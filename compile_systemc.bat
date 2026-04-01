@echo off
setlocal

REM 设置SystemC源码路径
set SYSTEMC_SRC=D:\TOOLS\systemc-2.3.3\systemc-2.3.3
set SYSTEMC_BUILD=%SYSTEMC_SRC%\build
set SYSTEMC_INSTALL=D:\TOOLS\systemc-2.3.3\install

echo 正在编译SystemC...

REM 创建构建目录
if not exist "%SYSTEMC_BUILD%" mkdir "%SYSTEMC_BUILD%"

REM 进入构建目录
cd /d "%SYSTEMC_BUILD%"

REM 运行CMake配置
cmake "%SYSTEMC_SRC%" -DCMAKE_INSTALL_PREFIX="%SYSTEMC_INSTALL%" -DCMAKE_BUILD_TYPE=Release

if %ERRORLEVEL% NEQ 0 (
    echo CMake配置失败
    exit /b %ERRORLEVEL%
)

REM 编译SystemC
cmake --build . --config Release

if %ERRORLEVEL% NEQ 0 (
    echo SystemC编译失败
    exit /b %ERRORLEVEL%
)

REM 安装SystemC
cmake --install .

if %ERRORLEVEL% NEQ 0 (
    echo SystemC安装失败
    exit /b %ERRORLEVEL%
)

echo SystemC编译安装完成!
echo 安装路径: %SYSTEMC_INSTALL%

REM 设置环境变量
setx SYSTEMC_HOME "%SYSTEMC_INSTALL%"
setx SYSTEMC_INCLUDE "%SYSTEMC_INSTALL%\include"
setx SYSTEMC_LIBDIR "%SYSTEMC_INSTALL%\lib-win64"

echo 环境变量已设置
echo 请重新启动终端以使环境变量生效