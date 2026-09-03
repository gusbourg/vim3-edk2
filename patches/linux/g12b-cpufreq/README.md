# g12b-cpufreq — CPU DVFS for ACPI-mode boots

Out-of-tree module giving the A73 cluster real frequency scaling under ACPI,
where no stock path exists: `cpufreq-dt` needs the clk/regulator/pinctrl phandle
graph, and `cppc_cpufreq` needs a PCC mailbox serviced by an always-on agent this
SoC does not have (no PCCT, no `_CPC`).

It therefore programs the hardware directly:

    sys_pll  0xff63c2f4   A73 cluster PLL (NOT sys1_pll — the clusters are crossed)
    mux      0xff63c208   HHI_SYS_CPUB_CLK_CNTL bit 11: 0 = dyn dividers, 1 = sys_pll
    PWM_A    0xffd1b000   VDDCPU_A, duty = hi/(hi+lo), V = 690mV + (1-duty)*360mV
    pinmux   0xff800018   GPIOE_2 bits 24-27 = 3 (pwm_a_e)

## Status

Binds via ACPI `PRP0001` against the DSDT `CPUF` device, whose `_CRS` supplies
the three regions above **in that order** (the driver indexes them). Soak-tested:
5 OPPs 1000–2208 MHz all within 0.15%; 120 s six-core load, 916 hashes
0 mismatches, 492 transitions, 64.5 °C plateau.

Install with `depmod`; udev autoloads it from the OF modalias
(`of:N*T*Camlogic,g12b-cpufreq`) that PRP0001 devices emit.

Remaining: the A53 cluster is untouched — still 1200 MHz vs 1800 rated, and
~58 mV undervolted.

## Do not remove the read-back in mux_select()

Parking the mux and asserting `PLL_RST` back-to-back hangs the SoC hard. The
write is ordered but the mux has not physically switched yet, so the PLL goes
into reset while the cluster is still sourced from it — and `target_index` runs
on a kworker inside that cluster. This hung the board three times.
