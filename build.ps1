# 独立运行的构建脚本：source ESP-IDF 环境并编译 photo_mcu
$ErrorActionPreference = 'Continue'
Set-ExecutionPolicy -Scope Process Bypass -Force | Out-Null
. D:\esp-idf\Espressif\frameworks\esp-idf-v5.2.7\export.ps1 *> $null
# 关闭 ccache：之前强杀构建进程遗留了锁文件，会导致编译一直卡死
$env:IDF_CCACHE_ENABLE = '0'
Set-Location D:\esp32\photo_mcu
idf.py --no-ccache build *> D:\esp32\photo_mcu\build_log.txt
"EXIT_CODE=$LASTEXITCODE" | Out-File -Append D:\esp32\photo_mcu\build_log.txt
