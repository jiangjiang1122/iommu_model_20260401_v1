#!/bin/bash
# 用日志 [PARSER_CONC] 逐请求快照验证: 写满时读并发表现; 并用 accept->release 重构对齐日志口径
set -u
export LC_ALL=C
LOG=/tmp/mdmc_rwsplit_n16c8.log
OUT=/tmp/task2_parser_conc.txt
: > "$OUT"

echo "===== [1] PARSER_CONC 快照统计 (模型自身计数器, 每任务弹出时刻) =====" >> "$OUT"
# 字段: t=.. task_id=.. dir=R/W read_out=.. write_out=.. global_out=.. R_FULL W_FULL G_FULL EITHER_FULL SELF_FULL OTHER_FULL
grep -o 'PARSER_CONC.*' "$LOG" > /tmp/pc_lines.txt
wc -l /tmp/pc_lines.txt >> "$OUT"

awk '{
  t=$1; sub("t=","",t); t+=0;
  dir=$3; sub("dir=","",dir);
  ro=$4; sub("read_out=","",ro); ro+=0;
  wo=$5; sub("write_out=","",wo); wo+=0;
  n++;
  if(dir=="R") nr++; else nw++;
  # 全程分布
  if(wo>=320){ wf++; rsum_wf+=ro; if(ro>rmx_wf) rmx_wf=ro;
    if(t<580347){ wf_inj++; rsum_wf_inj+=ro; if(ro>rmx_wf_inj) rmx_wf_inj=ro; }
  }
  if(ro>=320){ rf++; wsum_rf+=wo; }
  if(ro>=320 && wo>=320) both++;
  if(wo>=320 && ro<200) wf_rlow++;
  # 时间加权(快照间隔近似均匀, 用样本均值)
  rs+=ro; ws+=wo;
}
END{
  printf "快照总数=%d (R=%d W=%d)\n", n, nr, nw;
  printf "快照均值: read_out=%.1f write_out=%.1f\n", rs/n, ws/n;
  printf "write_out>=320 的快照: %d (%.2f%%), 其中读并发avg=%.1f max=%.0f\n", wf, 100*wf/n, rsum_wf/wf, rmx_wf;
  printf "  (限注入期 t<580347): n=%d, 读并发avg=%.1f max=%.0f\n", wf_inj, rsum_wf_inj/wf_inj, rmx_wf_inj;
  printf "  其中 read_out<200 的占比: %d/%d = %.1f%%\n", wf_rlow, wf, 100*wf_rlow/wf;
  printf "read_out>=320 的快照: %d (%.2f%%), 其中写并发avg=%.1f\n", rf, 100*rf/n, wsum_rf/rf;
  printf "读写同时>=320: %d (%.2f%%)\n", both, 100*both/n;
}' /tmp/pc_lines.txt >> "$OUT"

echo "" >> "$OUT"
echo "===== [2] FULL 标志计数 =====" >> "$OUT"
for f in R_FULL=1 W_FULL=1 G_FULL=1 EITHER_FULL=1 SELF_FULL=1 OTHER_FULL=1; do
  c=$(grep -c "$f" /tmp/pc_lines.txt)
  echo "$f : $c" >> "$OUT"
done

echo "" >> "$OUT"
echo "===== [3] CSV accept->release 口径重构 (对齐 SPLIT=1 端口计数) =====" >> "$OUT"
CSV=/mnt/d/Qoder_proj/iommu_model_20260401_v1/multidev_mc_rwsplit_n16_c8_512mb_requests.csv
awk -F',' 'NR>1 && $1==3 {
  w=$7; printf "%.1f %d %d\n", $16, w, 1;
  printf "%.1f %d %d\n", $22, w, -1;
}' "$CSV" | sort -n > /tmp/rw_events2.txt
awk '{
  t=$1; w=$2; d=$3;
  if(NR>1 && t>t_prev){
    dt=t-t_prev;
    rs+=rc*dt; ws+=wc*dt; ts+=dt;
    if(rc>=320) trf+=dt;
    if(wc>=320) twf+=dt;
    if(rc>=320&&wc>=320) tbf+=dt;
    if(rc>rmx) rmx=rc; if(wc>wmx) wmx=wc;
    if(wc>=320){ rwf+=rc*dt; }
    if(rc>=320){ wrf+=wc*dt; }
  }
  t_prev=t;
  if(w==0) rc+=d; else wc+=d;
}
END{
  printf "窗口=%.0fns\n", t_prev;
  printf "Read : avg=%.2f (%.1f%%) 满320时长=%.1f%% max=%.0f\n", rs/ts, 100*rs/ts/320, 100*trf/ts, rmx;
  printf "Write: avg=%.2f (%.1f%%) 满320时长=%.1f%% max=%.0f\n", ws/ts, 100*ws/ts/320, 100*twf/ts, wmx;
  printf "同时满: %.2f%%\n", 100*tbf/ts;
  printf "写满期间读并发avg=%.1f | 读满期间写并发avg=%.1f\n", rwf/twf, wrf/trf;
}' /tmp/rw_events2.txt >> "$OUT"

echo "" >> "$OUT"
echo "===== [4] 方向分解: W(accept->release)均值 + lambda*W 对账 =====" >> "$OUT"
awk -F',' 'NR>1 && $1==3 {
  w=$7; dt=$22-$16;
  if(w==0){ rn++; rsum+=dt; if($16<rfirst||rfirst==0)rfirst=$16; if($22>rlast)rlast=$22; }
  else { wn++; wsum+=dt; if($16<wfirst||wfirst==0)wfirst=$16; if($22>wlast)wlast=$22; }
}
END{
  rw = rlast-rfirst; ww = wlast-wfirst;
  printf "READ : n=%d 活跃跨度=%.0fns lambda=%.2fM/s W_avg=%.1fns => L=lambda*W=%.1f\n", rn, rw, rn/rw/1000.0, rsum/rn, (rn/rw)*(rsum/rn);
  printf "WRITE: n=%d 活跃跨度=%.0fns lambda=%.2fM/s W_avg=%.1fns => L=lambda*W=%.1f\n", wn, ww, wn/ww/1000.0, wsum/wn, (wn/ww)*(wsum/wn);
  printf "窗口口径L(全程726690ns): R=%.1f W=%.1f\n", rn*(rsum/rn)/726690, wn*(wsum/wn)/726690;
}' "$CSV" >> "$OUT"

echo PARSER_CONC_DONE >> "$OUT"
cat "$OUT"
