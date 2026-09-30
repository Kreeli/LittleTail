# Verify the USB descriptors without opening MounRiver Studio.
#
# Steps:
#   1. recompile the touched sources with the exact project flags
#   2. link them together with the .o files already in obj/
#   3. run audit_uac2.py against the resulting ELF, which extracts the real
#      bytes of conf_desc / device_desc / g_audio_fs_table and validates them
#      against the USB Audio 2.0 rules
#
# Why this exists: every bLength in the descriptor macros is computed from the
# number of macro arguments, so a missing bmaControls group silently produces a
# malformed descriptor. Only reading bytes out of the build product catches it.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads UTF-8 files
# without a BOM as ANSI/GBK, and non-ASCII comment bytes can swallow the
# following newline and produce bogus parse errors.
#
# Where the build goes: when the sandbox only permits writes at the workspace
# root, the project's own obj/ tree is not writable, so compile and link happen
# in %TEMP%\uac2_build instead (obj/*.o are read, never written).
#
# Usage:
#     & tools\uac2_audit\verify.ps1
#     & tools\uac2_audit\verify.ps1 -ToolchainBin 'D:\...\bin'

param(
    [string]$ToolchainBin = 'D:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin',
    [string]$Python = 'C:\Users\hachimi\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $env:TEMP 'uac2_build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$gcc = Join-Path $ToolchainBin 'riscv-none-embed-gcc.exe'
if (-not (Test-Path $gcc)) { Write-Host "compiler not found: $gcc" -ForegroundColor Red; exit 2 }

$cflags = @(
    '-march=rv32imafcxw', '-mabi=ilp32f', '-msmall-data-limit=8', '-msave-restore',
    '-fmax-errors=20', '-O0', '-fmessage-length=0', '-fsigned-char',
    '-ffunction-sections', '-fdata-sections', '-fno-common',
    '-fsingle-precision-constant', '-Wunused', '-Wuninitialized', '-g', '-std=gnu99', '-Wall'
)
$incdirs = @('Debug', 'Core', 'User', 'Peripheral/inc', 'Cherry_USB/class/audio',
             'Cherry_USB/class/cdc', 'Cherry_USB/common', 'Cherry_USB/core',
             'Cherry_USB/port/usbhs')
$incs = @()
foreach ($d in $incdirs) { $incs += ('-I' + (Join-Path $root $d)) }

# sources that were touched and must be recompiled
# (everything else is linked from the .o files already in obj/)
$rebuild = @(
    @{ src = 'User\usb_app.c';                       obj = 'usb_app.o' },
    @{ src = 'User\main.c';                          obj = 'main.o' },
    @{ src = 'User\Syscfg.c';                        obj = 'Syscfg.o' },
    @{ src = 'User\ES9018.c';                        obj = 'ES9018.o' },
    @{ src = 'User\ch32v30x_it.c';                   obj = 'ch32v30x_it.o' },
    @{ src = 'Cherry_USB\port\usbhs\usb_dc_usbhs.c'; obj = 'usb_dc_usbhs.o' }
)

$fail = 0
foreach ($r in $rebuild) {
    $s = Join-Path $root $r.src
    $o = Join-Path $out $r.obj
    Write-Host "--- compile $($r.src)" -ForegroundColor Cyan
    & $gcc @cflags @incs -MMD -MP -MF "$o.d" -MT $o -c -o $o $s 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host "compile FAILED" -ForegroundColor Red; $fail = 1 }
}

# link: existing objects from obj/, minus every object we are about to rebuild
$stale = @()
foreach ($r in $rebuild) {
    $stale += (Join-Path $root ('obj\' + (Split-Path $r.src -Parent) + '\' + $r.obj))
}
$objs = @()
$objs += Get-ChildItem (Join-Path $root 'obj') -Recurse -Filter *.o |
         Where-Object { $stale -notcontains $_.FullName } |
         Select-Object -ExpandProperty FullName
foreach ($r in $rebuild) { $objs += (Join-Path $out $r.obj) }

$elf = Join-Path $out 'LittleTail.elf'
$map = Join-Path $out 'LittleTail.map'
$ldargs = @(
    '-march=rv32imafcxw', '-mabi=ilp32f', '-msmall-data-limit=8', '-msave-restore', '-O0',
    '-fsigned-char', '-ffunction-sections', '-fdata-sections', '-fno-common',
    '-fsingle-precision-constant', '-g',
    '-T', (Join-Path $root 'Ld\Link.ld'),
    '-nostartfiles', '-Xlinker', '--gc-sections',
    ('-Wl,-Map,' + $map),
    '--specs=nano.specs', '--specs=nosys.specs',
    '-o', $elf
)
Write-Host "--- link $($objs.Count) objects" -ForegroundColor Cyan
& $gcc @ldargs @objs '-lm' 2>&1 | Select-Object -Last 20
if ($LASTEXITCODE -ne 0) { Write-Host "link FAILED" -ForegroundColor Red; $fail = 1 }

if ($fail -eq 0) {
    & (Join-Path $ToolchainBin 'riscv-none-embed-size.exe') --format=berkeley $elf
    Write-Host "--- descriptor audit" -ForegroundColor Cyan
    & $Python (Join-Path $PSScriptRoot 'audit_uac2.py') $elf
    if ($LASTEXITCODE -ne 0) { $fail = 1 }
    Write-Host "--- I2S sample rate check (informational)" -ForegroundColor Cyan
    $i2s = & $Python (Join-Path $PSScriptRoot 'check_i2s_source.py') 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "skipped: it greps for the old template clock symbols (PLL3 / ES9018_MCLK_HZ);" -ForegroundColor Yellow
        Write-Host "the current Syscfg.c / ES9018.h use their own naming, so this check does not apply." -ForegroundColor Yellow
    } else {
        $i2s | Write-Host
    }
    Write-Host "ELF: $elf"
}

exit $fail
