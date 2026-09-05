Import("env")

import subprocess

RAM_BUDGET_BYTES = 65_536
FLASH_BUDGET_BYTES = 262_144


def check_firmware_budget(source, target, env):
    elf_path = env.subst("$BUILD_DIR/${PROGNAME}.elf")
    size_program = env.subst("$SIZETOOL")
    completed = subprocess.run(
        [size_program, "-A", "-d", elf_path],
        check=True,
        capture_output=True,
        text=True,
    )
    sections = {}
    for row in completed.stdout.splitlines():
        columns = row.split()
        if len(columns) >= 2 and columns[0].startswith("."):
            sections[columns[0]] = int(columns[1])
    if ".text" not in sections or ".data" not in sections or ".bss" not in sections:
        raise RuntimeError("could not parse firmware section sizes")

    flash_bytes = sum(
        sections.get(name, 0)
        for name in (".boot2", ".text", ".data", ".rodata", ".text.align", ".ARM.exidx")
    )
    ram_bytes = sum(
        sections.get(name, 0) for name in (".data", ".bss", ".noinit")
    )
    print(
        "Firmware budget: "
        f"RAM {ram_bytes}/{RAM_BUDGET_BYTES} bytes, "
        f"flash {flash_bytes}/{FLASH_BUDGET_BYTES} bytes"
    )
    if ram_bytes > RAM_BUDGET_BYTES:
        raise RuntimeError(
            f"firmware RAM use {ram_bytes} exceeds {RAM_BUDGET_BYTES}-byte budget"
        )
    if flash_bytes > FLASH_BUDGET_BYTES:
        raise RuntimeError(
            f"firmware flash use {flash_bytes} exceeds {FLASH_BUDGET_BYTES}-byte budget"
        )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_firmware_budget)
