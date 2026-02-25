param(
    [string]$Gdb = "D:/appdata/users/qwe17/zephyr-sdk-0.17.4/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-gdb.exe",
    [string]$Elf = "D:/Projects/zephyr-ot-clean/walle_g_build/zephyr/zephyr.elf",
    [string]$GdbTarget = "localhost:2331",
    [string]$BareMetalRoot = "D:/Projcets/walle_g",
    [switch]$WriteTest,
    [switch]$AfterMain
)

function Parse-Defines {
    param([string]$Path)

    $defs = @{}
    Get-Content -Path $Path | ForEach-Object {
        if ($_ -match '^\s*#define\s+([A-Za-z0-9_]+)\s+([0-9A-Fa-fxX]+)\b') {
            $defs[$matches[1]] = $matches[2]
        }
    }
    return $defs
}

function To-UInt64 {
    param([string]$Value)

    if ($Value -match '^0[xX]') {
        return [Convert]::ToUInt64($Value.Substring(2), 16)
    }
    return [Convert]::ToUInt64($Value, 10)
}

function Hex32 {
    param([UInt64]$Value)
    return ("0x{0:X8}" -f $Value)
}

$socPath = Join-Path $BareMetalRoot "src/bsp/efinix/EfxSapphireSoc/include/soc.h"
$clintPath = Join-Path $BareMetalRoot "src/driver/clint.h"

if (-not (Test-Path $socPath)) {
    Write-Output "ERROR: soc.h not found: $socPath"
    exit 3
}

if (-not (Test-Path $clintPath)) {
    Write-Output "ERROR: clint.h not found: $clintPath"
    exit 3
}

$socDefs = Parse-Defines -Path $socPath
$clintDefs = Parse-Defines -Path $clintPath

$requiredSoc = @("SYSTEM_CLINT_CTRL", "SYSTEM_UART_0_IO_CTRL", "SYSTEM_PLIC_CTRL")
$requiredClint = @("CLINT_TIME_ADDR", "CLINT_CMP_ADDR")

foreach ($k in $requiredSoc) {
    if (-not $socDefs.ContainsKey($k)) {
        Write-Output "ERROR: missing define in soc.h: $k"
        exit 3
    }
}
foreach ($k in $requiredClint) {
    if (-not $clintDefs.ContainsKey($k)) {
        Write-Output "ERROR: missing define in clint.h: $k"
        exit 3
    }
}

$clintBase = To-UInt64 $socDefs["SYSTEM_CLINT_CTRL"]
$uartBase = To-UInt64 $socDefs["SYSTEM_UART_0_IO_CTRL"]
$plicBase = To-UInt64 $socDefs["SYSTEM_PLIC_CTRL"]
$mtimeOff = To-UInt64 $clintDefs["CLINT_TIME_ADDR"]
$cmpOff = To-UInt64 $clintDefs["CLINT_CMP_ADDR"]

$mtimeLow = $clintBase + $mtimeOff
$mtimeHigh = $mtimeLow + 4
$mtimecmpLow = $clintBase + $cmpOff
$mtimecmpHigh = $mtimecmpLow + 4
$uartClockDiv = $uartBase + 8

$cmd = "$env:TEMP/walle_probe_mmio.gdb"
$log = "$env:TEMP/walle_probe_mmio.log"

$gdbScript = @(
    "set pagination off",
    "set confirm off",
    "set breakpoint pending on",
    "set remotetimeout 3",
    "",
    "file $Elf",
    "target extended-remote $GdbTarget",
    "monitor reset",
    "monitor halt"
)

if ($AfterMain) {
    $gdbScript += @(
        "",
        "echo \n=== RUN TO MAIN FIRST ===\n",
        "tbreak main",
        "continue",
        "monitor halt"
    )
}

$gdbScript += @(
    "",
    "echo \n=== MMIO PROBE (READ, from bare-metal headers) ===\n",
    "echo [CLINT] MTIME low  $(Hex32 $mtimeLow)\n",
    "monitor MemU32 $(Hex32 $mtimeLow)",
    "echo [CLINT] MTIME high $(Hex32 $mtimeHigh)\n",
    "monitor MemU32 $(Hex32 $mtimeHigh)",
    "echo [CLINT] CMP low    $(Hex32 $mtimecmpLow)\n",
    "monitor MemU32 $(Hex32 $mtimecmpLow)",
    "echo [CLINT] CMP high   $(Hex32 $mtimecmpHigh)\n",
    "monitor MemU32 $(Hex32 $mtimecmpHigh)",
    "",
    "echo [UART ] BASE       $(Hex32 $uartBase)\n",
    "monitor MemU32 $(Hex32 $uartBase)",
    "echo [UART ] CLOCK/DIV  $(Hex32 $uartClockDiv)\n",
    "monitor MemU32 $(Hex32 $uartClockDiv)",
    "",
    "echo [PLIC ] BASE       $(Hex32 $plicBase)\n",
    "monitor MemU32 $(Hex32 $plicBase)",
    "",
    "echo [RAM  ] TEXT BASE  0x10000000\n",
    "monitor MemU32 0x10000000"
)

if ($WriteTest) {
    $gdbScript += @(
        "",
        "echo \n=== MMIO PROBE (WRITE) ===\n",
        "echo [UART ] CLOCK/DIV <= 0x0000000A @ $(Hex32 $uartClockDiv)\n",
        "monitor WriteU32 $(Hex32 $uartClockDiv) 0x0000000A",
        "monitor MemU32 $(Hex32 $uartClockDiv)",
        "",
        "echo [CLINT] CMP low   <= 0xFFFFFFFF @ $(Hex32 $mtimecmpLow)\n",
        "monitor WriteU32 $(Hex32 $mtimecmpLow) 0xFFFFFFFF",
        "monitor MemU32 $(Hex32 $mtimecmpLow)"
    )
}

$gdbScript += @(
    "",
    "echo \n=== END PROBE ===\n",
    "monitor halt",
    "quit"
)

$gdbScript | Set-Content -Encoding ascii $cmd

Write-Output "Running MMIO probe via GDB batch..."
Write-Output "  gdb: $Gdb"
Write-Output "  elf: $Elf"
Write-Output "  target: $GdbTarget"
Write-Output "  soc.h: $socPath"
Write-Output "  clint.h: $clintPath"
Write-Output "  clint base: $(Hex32 $clintBase), mtime off: $(Hex32 $mtimeOff), cmp off: $(Hex32 $cmpOff)"
Write-Output "  uart base:  $(Hex32 $uartBase)"
Write-Output "  plic base:  $(Hex32 $plicBase)"
if ($WriteTest) {
    Write-Output "  mode: read + write test"
} else {
    Write-Output "  mode: read only"
}
if ($AfterMain) {
    Write-Output "  stage: probe after hitting main"
} else {
    Write-Output "  stage: probe right after reset/halt"
}
Write-Output ""

& $Gdb -q -batch -x $cmd 2>&1 | Tee-Object -FilePath $log
$rc = $LASTEXITCODE

$failedCount = (Select-String -Path $log -Pattern "\(Failed\)" | Measure-Object).Count

Write-Output ""
Write-Output "Probe log saved to: $log"
Write-Output "GDB exit code: $rc"
Write-Output "MMIO read failures: $failedCount"

if ($rc -eq 0 -and $failedCount -gt 0) {
    exit 2
}

exit $rc
