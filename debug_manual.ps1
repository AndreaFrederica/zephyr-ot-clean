$gdb = "D:/appdata/users/qwe17/zephyr-sdk-0.17.4/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-gdb.exe"
$elf = "D:/Projects/zephyr-ot-clean/walle_g_build/zephyr/zephyr.elf"
$cmd = "$env:TEMP/walle_manual_interactive.gdb"

@'
set pagination off
set confirm off
set breakpoint pending on
set disassemble-next-line on
set remotetimeout 10

file D:/Projects/zephyr-ot-clean/walle_g_build/zephyr/zephyr.elf
target extended-remote localhost:2331
monitor reset
monitor halt
load
monitor halt

if $pc == 0xdeadbeef
  printf "\nERROR: target not halted correctly (PC=0xdeadbeef). Restart J-Link GDB Server and retry.\n"
end

delete breakpoints

# Boot handoff checkpoints (single-shot)
tbreak z_cstart
tbreak *z_cstart+0x20
tbreak *z_cstart+0x24
tbreak prepare_multithreading
tbreak switch_to_main_thread
tbreak bg_thread_main
tbreak z_thread_entry

# Loop breakpoint in hello_world main
break zephyr/samples/hello_world/src/main.c:17

# Fatal path breakpoints
hbreak z_fatal_error
hbreak k_sys_fatal_error_handler
hbreak arch_system_halt

define hook-stop
  printf "\n---- STOP ----\n"
  info reg pc ra sp mepc mcause mtval mstatus mie mip
  x/i $pc
  if $mepc != 0
    x/i $mepc
  end
end

define bye
  echo \n[bye] halting and detaching...\n
  interrupt
  monitor halt
  detach
  quit
end

printf "\nReady. You control GDB now.\n"
printf "Tips: c=continue, n=next, si=stepi, info b=breakpoints, bye=clean exit\n\n"
'@ | Set-Content -Encoding ascii $cmd

& $gdb -q -x $cmd
