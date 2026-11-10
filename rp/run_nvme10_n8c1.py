#!/usr/bin/env python3
"""在WSL内隔离构建、仿真十包组，并回归原n8c2；不覆盖历史产物。"""
import argparse
import datetime
import gzip
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--regression-only', action='store_true')
    ap.add_argument('--devices', type=int, choices=(8,16), default=8)
    ap.add_argument('--pt-update-fifo', type=int, choices=(32,1024), default=None,
                    help='仅n16：不指定=沿用模型默认32，与n8c1逐宏一致；1024=复现既往扩容实验')
    ap.add_argument('--groups', type=int, choices=(125,250), default=None,
                    help='每设备组数；默认n8=250、n16=125（激励减半，总包数仍为20000）')
    ap.add_argument('--agg-flush-order', type=int, choices=(0,1), default=None,
                    help='agg_flush写出顺序：不指定=用模型默认1(释放优先)；0=旧交替顺序，用于A/B复现死锁')
    ap.add_argument('--tag', default='', help='构建/二进制名后缀，用于隔离模型改动前后的产物')
    ap.add_argument('--run-id', default=datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    args = ap.parse_args()
    if args.devices == 16 and args.regression_only:
        ap.error('16设备扩容只运行新场景，不自动重测历史基线')
    if args.devices == 8 and args.pt_update_fifo is not None:
        ap.error('PT更新FIFO覆盖仅限n16实验，禁止改变n8基线配置')
    if args.devices == 8 and args.groups is not None:
        ap.error('n8基线固定每设备250组，禁止改变基线激励规模')
    # 32即模型默认深度，不注入宏，保证与n8基线的编译宏集合完全相同。
    fifo_depth = args.pt_update_fifo or 32
    inject_fifo = args.pt_update_fifo == 1024
    groups = args.groups or (125 if args.devices == 16 else 250)
    root = Path(__file__).resolve().parents[1]
    scene = 'n%dc1' % args.devices
    archive = root / 'rwsplit_results' / ('nvme10_' + scene + '_' + args.run_id)
    work = Path('/tmp') / ('nvme10_' + scene + '_' + args.run_id)
    archive.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    # n16 始终带 _g<组数> 后缀，避免覆盖既往归档所引用的对象目录与二进制。
    suffix = ('_fifo%d' % fifo_depth if inject_fifo else '')
    if args.devices == 16 or groups != 250:
        suffix += '_g%d' % groups
    if args.agg_flush_order is not None:
        suffix += '_rel%d' % args.agg_flush_order
    if args.tag:
        suffix += '_' + args.tag
    arms = [(scene, 'rand4k_nvme10_multidev_mc_' + scene,
             'build_mdmc_' + scene + '_nvme10' + suffix,
             'iommu_model_mdmc_' + scene + '_nvme10' + suffix)]
    if args.regression_only:
        arms = [('n8c2_regression', 'rand4k_msi_mix_multidev_mc_n8c2',
                 'build_mdmc_n8c2_nvme10_reg', 'iommu_model_mdmc_n8c2_nvme10_reg')]
    for label, test, build, binary in arms:
        out = archive / label
        run = work / label
        out.mkdir(exist_ok=False)
        run.mkdir(exist_ok=False)
        config = root / 'iommu/cache_config/default_config.json'
        (run / 'iommu/cache_config').mkdir(parents=True)
        shutil.copy2(config, run / 'iommu/cache_config/default_config.json')
        shutil.copy2(config, out / 'cache_config_snapshot.json')
        command = ['make', '-B', '-j6', 'TEST=' + test,
                   'BUILD_ROOT=' + build, 'TARGET=' + binary,
                   'MDMC81_GROUPS=%d' % groups]
        if inject_fifo:
            command.append('MDMC10_PT_UPDATE_FIFO=%d' % fifo_depth)
        if args.agg_flush_order is not None:
            command.append('MDMC10_AGG_FLUSH_ORDER=%d' % args.agg_flush_order)
        # 模型侧改动的可审计记录：agg_flush 写出顺序 + 相关源码哈希。
        model_files = ['iommu/iommu_perf_model/iommu_perf_ptw.cc',
                       'iommu/iommu_perf_model/iommu_perf_params.hh']
        model_info = {
            'agg_flush_release_first': 1 if args.agg_flush_order is None else args.agg_flush_order,
            'agg_flush_order_explicit': args.agg_flush_order is not None,
            'source_sha256': {n: hashlib.sha256((root / n).read_bytes()).hexdigest()
                              for n in model_files}}
        (out / 'model_info.json').write_text(json.dumps(model_info, indent=2), encoding='utf-8')
        if args.devices == 16:
            baseline = root / 'rwsplit_results/nvme10_n8c1_20261029_fit10/n8c1'
            old = json.loads((baseline / 'input_manifest.json').read_text())
            for name in ('rp/test_rp_rand4k_msi_mix_multidev_mc_thread.cc',
                         'iommu/cache_config/default_config.json'):
                assert hashlib.sha256((root / name).read_bytes()).hexdigest() == old['source_sha256'][name], \
                    '基线源码或Cache配置已变化，不能视为仅改激励：' + name
            probe = subprocess.run(command + ['-n'], cwd=root, capture_output=True, text=True, check=True)
            flags = set(re.findall(r'-D[^\s]+', probe.stdout))
            old_flags = set(re.findall(r'-D[^\s]+', (baseline / 'build.log').read_text()))
            # 只允许“设备数”和“每设备组数”两项激励宏不同；硬件配置宏必须逐位一致。
            expected = (old_flags
                        - {'-DTEST_CFG_NUM_DEVICES=8', '-DTEST_CFG_NUM_PAGES=250'}
                        | {'-DTEST_CFG_NUM_DEVICES=16', '-DTEST_CFG_NUM_PAGES=%d' % groups})
            if inject_fifo:
                expected |= {'-DTEST_CFG_PT_UPDATE_FIFO_DEPTH=%d' % fifo_depth}
            else:
                assert not any(f.startswith('-DTEST_CFG_PT_UPDATE_FIFO_DEPTH') for f in old_flags | flags), \
                    '未指定覆盖却出现PT更新FIFO宏，n16将与n8基线不同配置'
            if args.agg_flush_order is not None:
                expected |= {'-DTEST_CFG_AGG_FLUSH_RELEASE_FIRST=%d' % args.agg_flush_order}
            else:
                assert not any(f.startswith('-DTEST_CFG_AGG_FLUSH_RELEASE_FIRST') for f in flags), \
                    '未显式指定却注入写出顺序宏，默认口径应保持宏集合不变'
            assert flags == expected, '激励宏之外的编译宏发生变化：' + str(flags ^ expected)
            changed = ['TEST_CFG_NUM_DEVICES=8 -> 16（激励）',
                       'TEST_CFG_NUM_PAGES=250 -> %d（激励，每设备组数）' % groups]
            if inject_fifo:
                changed.append('TEST_CFG_PT_UPDATE_FIFO_DEPTH=32(模型默认) -> %d（硬件配置，不同构）' % fifo_depth)
            if args.agg_flush_order is not None:
                changed.append('TEST_CFG_AGG_FLUSH_RELEASE_FIRST=1(模型默认) -> %d（模型写出顺序，A/B用）'
                               % args.agg_flush_order)
            # 只有既未覆盖FIFO深度、也未覆盖agg_flush写出顺序时，才算纯激励差异。
            stimulus_only = not inject_fifo and args.agg_flush_order is None
            (out / 'config_delta.json').write_text(json.dumps({
                'changed': changed,
                'stimulus_only': stimulus_only,
                'same_hardware_config_as_n8_baseline': stimulus_only,
                'pt_update_fifo_depth': fifo_depth,
                'groups_per_device': groups,
                'expected_packets': args.devices * groups * 10,
                'model_change': model_info,
                'shared_thread_sha256_match': True, 'cache_config_sha256_match': True,
                'prior_experiments': {
                    'fifo32_groups250_deadlock': 'nvme10_n16c1_20261029_expand16/n16c1',
                    'fifo1024_groups250_passed': 'nvme10_n16c1_20261029_expand16_fix1024/n16c1'},
                'old_flags': sorted(old_flags), 'new_flags': sorted(flags)}, indent=2))
            print('CONFIG_DELTA_OK devices=8->16 groups=250->%d fifo=%d release_first=%s stimulus_only=%s'
                  % (groups, fifo_depth,
                     1 if args.agg_flush_order is None else args.agg_flush_order, stimulus_only), flush=True)
        print('BUILD_START', label, 'ARCHIVE', out, flush=True)
        with (out / 'build.log').open('w') as f:
            result = subprocess.run(command, cwd=root, stdout=f, stderr=subprocess.STDOUT)
        print('BUILD_RC', result.returncode, flush=True)
        if result.returncode:
            print('\n'.join((out / 'build.log').read_text(errors='replace').splitlines()[-35:]))
            return result.returncode
        manifest = {'test': test, 'command': command, 'run_dir': str(run),
                    'binary': str(root / binary), 'source_sha256': {}}
        for name in ['Makefile', 'rp/test_rp_rand4k_msi_mix_multidev_mc_thread.cc',
                     'iommu/cache_config/default_config.json'] + model_files + [binary]:
            manifest['source_sha256'][name] = hashlib.sha256((root / name).read_bytes()).hexdigest()
        (out / 'input_manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        print('SIM_START', run, flush=True)
        with (run / 'sim.log').open('w') as f:
            try:
                result = subprocess.run([str(root / binary)], cwd=run, stdout=f,
                                        stderr=subprocess.STDOUT, timeout=900)
                rc = result.returncode
            except subprocess.TimeoutExpired:
                rc = 124
        print('SIM_RC', rc, flush=True)
        # 保留末尾统计段，完整大日志压缩归档。
        extractor = root / '.qoder/skills/iommu-perf-stats/scripts/extract_iommu_perf.py'
        sys.path.insert(0, str(extractor.parent))
        from extract_iommu_perf import tail_lines
        tail = '\n'.join(tail_lines(run / 'sim.log', 20000))
        (out / 'sim_tail.txt').write_text(tail, encoding='utf-8')
        for p in run.iterdir():
            if p.suffix not in ('.csv', '.log'):
                continue
            if p.name == 'sim.log' or p.stat().st_size > 2_000_000:
                with p.open('rb') as src, gzip.open(out / (p.name + '.gz'), 'wb') as dst:
                    shutil.copyfileobj(src, dst)
            else:
                shutil.copy2(p, out / p.name)
        for line in tail.splitlines():
            if any(k in line for k in ('[MULTIDEV_MC_RESULT]', '[MD_FAIRNESS]',
                                       '[MDMC_DELTA]', 'Steady IOPS:', 'Fatal:')):
                print(line, flush=True)
        if rc or '[MULTIDEV_MC_RESULT] PASS' not in tail:
            print('仿真未完成，保留现场，禁止将截断日志当成功结果', flush=True)
            return rc or 1
        subprocess.run([sys.executable, str(extractor), str(run / 'sim.log'),
                        '--json', str(out / 'perf_raw.json'), '--md', str(out / 'perf_raw.md')],
                       stdout=subprocess.DEVNULL, check=True)
        if label == scene:
            subprocess.run([sys.executable, str(root / 'rp/analyze_nvme10_n8c1.py'),
                            '--devices', str(args.devices), '--groups', str(groups),
                            '--pt-update-fifo', str(fifo_depth),
                            '--run-dir', str(run), '--out-dir', str(out)], check=True)
        print('ARM_COMPLETE', label, out, flush=True)
        if label == 'n8c2_regression' and (archive / 'n8c1/perf_raw.json').exists():
            subprocess.run([sys.executable, str(root / 'rp/analyze_nvme10_n8c1.py'),
                            '--run-dir', str(work / 'n8c1'),
                            '--out-dir', str(archive / 'n8c1')], check=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
