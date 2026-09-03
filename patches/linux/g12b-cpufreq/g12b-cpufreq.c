// SPDX-License-Identifier: GPL-2.0
/*
 * Amlogic G12B (A311D) CPU frequency scaling for ACPI-mode boots.
 *
 * ACPI cannot express the clock/regulator/pinctrl phandle graph that
 * cpufreq-dt relies on, and the standard ARM64 ACPI path (CPPC) needs a PCC
 * mailbox serviced by an always-on agent that this SoC does not have.  So this
 * driver bypasses the clk/regulator/pwm frameworks entirely and pokes the
 * three register blocks directly, exactly as measured from a working
 * DeviceTree boot.
 *
 * Big cluster (A73, cpu2-5) only for now.  The little cluster runs the boot
 * CPU, so reprogramming its PLL needs the dyn-mux parking dance performed from
 * a core that is not itself clocked by it.
 *
 * Binds via ACPI PRP0001 against the DSDT CPUF device, whose _CRS carries the
 * three register blocks in a fixed order (HHI, PWM_AB, AO pinmux).  The same
 * of_match_table also makes it bind on DeviceTree, though there cpufreq-dt is
 * the better choice and this driver should not be used.
 */

#include <linux/arch_topology.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* HHI (clock controller) */
#define HHI_SYS_PLL_CNTL0		0x2f4	/* feeds the A73 cluster */
#define HHI_SYS_CPUB_CLK_CNTL		0x208	/* A73 cluster mux */
#define HHI_SYS_CPU_CLK_CNTL0		0x19c	/* A53 cluster mux */
#define HHI_SYS1_PLL_CNTL0		0x380	/* feeds the A53 cluster */

#define PLL_M_MASK			GENMASK(7, 0)
#define PLL_N_MASK			GENMASK(14, 10)
#define PLL_OD_MASK			GENMASK(18, 16)
#define PLL_EN				BIT(28)
#define PLL_RST				BIT(29)
#define PLL_LOCK			BIT(31)
#define PLL_M_MIN			128
#define PLL_M_MAX			250

#define CLK_MUX_PLL			BIT(11)	/* 0 = dyn dividers, 1 = the PLL */

/* cbus PWM_AB - channel A drives VDDCPU_A */
#define REG_PWM_A			0x00
#define REG_PWM_B			0x04
#define REG_MISC_AB			0x08
#define MISC_A_EN			BIT(0)
#define MISC_A_CLK_EN			BIT(15)
#define MISC_A_CLK_SEL_SHIFT		4
#define MISC_A_CLK_SEL_FCLK_DIV3	3

/* AO pinmux - GPIOE_2 must be function 3 (pwm_a_e) or the rail is unmanaged */
#define GPIOE_2_SHIFT			24
#define GPIOE_2_PWM_A_E			3

/*
 * PWM period is 1250 ns off fclk_div3 (666.67 MHz) => 833 counts, matching the
 * DT pwm-regulator.  Rail is linear and INVERTED: duty 0% = VMAX, 100% = VMIN.
 */
#define PWM_TOTAL_CNT			833
#define VMIN_UV				690000
#define VMAX_UV				1050000

/* The dyn-divider path tops out here; above it we must use the PLL. */
#define DYN_PATH_KHZ			1000000
#define XTAL_KHZ			24000

struct g12b_opp {
	unsigned int khz;
	unsigned int uv;
};

/*
 * One per cluster.  The two differ in every register they touch: separate PLLs,
 * separate cluster muxes, and rails on two different PWM controllers with
 * different periods.
 */
struct g12b_cluster {
	const char		*name;
	unsigned int		mux_off;	/* HHI offset of the cluster mux */
	unsigned int		pll_off;	/* HHI offset of its PLL */
	void __iomem		*pwm;		/* PWM block base (may be NULL) */
	unsigned int		pwm_reg;	/* REG_PWM_A or REG_PWM_B */
	unsigned int		pwm_total;	/* hi+lo giving a 1250 ns period */
	unsigned int		pwm_clk_sel;	/* MISC clk_sel picking that parent */
	unsigned int		pwm_en, pwm_clk_en;	/* MISC enable bits */
	unsigned int		pwm_sel_shift, pwm_div_shift;
	const struct g12b_opp	*opps;
	unsigned int		nopps;
	unsigned int		first_cpu, last_cpu;
	unsigned int		dyn_khz;	/* rate of the dyn path, 0 = unusable */
	unsigned int		pll_khz;	/* what the PLL is programmed to */
	struct cpufreq_frequency_table *ft;
};

/*
 * Only rates the PLL can synthesise exactly with n=1, od=1 (f = 12 MHz * m,
 * m in [128,250]), plus 1000 MHz from the dyn path.  Voltages are the mainline
 * A311D cpub OPP table values, each one confirmed against the PWM duty a real
 * DT boot programs.
 */
/*
 * A73.  Rates and voltages are mainline's cpub table, restricted to what the
 * PLL can synthesise exactly (f = 24 MHz * m / 2^od, n=1, m in [128,250]).
 *
 * 1512 and 2108 are omitted deliberately: DT advertises them but can only
 * approximate them - it programs m=250/od=2 (1500 MHz) and m=175/od=1
 * (2100 MHz) respectively.  2304 and 2400 are omitted as overclock above
 * mainline's 2208 ceiling.
 */
static const struct g12b_opp a73_opps[] = {
	{ 1000000,  731000 },
	{ 1200000,  751000 },
	{ 1398000,  771000 },
	{ 1608000,  781000 },
	{ 1704000,  791000 },
	{ 1800000,  831000 },
	{ 1908000,  861000 },
	{ 2016000,  911000 },
	{ 2208000, 1011000 },
};

/*
 * A53.  1512 MHz is absent on purpose: it needs m=126 at od=1 (below the 128
 * minimum) or m=252 at od=2 (above the 250 maximum), so the PLL cannot
 * synthesise it with n=1.
 *
 * The bottom entry is 1008 MHz, not 1000: this cluster has no usable dyn path
 * (see dyn_khz below), so every rate must come from the PLL, and 1000 MHz is
 * not synthesisable while 1008 is (m=168, od=2).
 */
static const struct g12b_opp a53_opps[] = {
	{ 1008000,  761000 },
	{ 1200000,  781000 },
	{ 1398000,  811000 },
	{ 1608000,  901000 },
	{ 1704000,  951000 },
	{ 1800000, 1001000 },
};

static struct g12b_cluster clusters[] = {
	{
		.name = "A73", .mux_off = HHI_SYS_CPUB_CLK_CNTL,
		.pll_off = HHI_SYS_PLL_CNTL0, .pwm_reg = REG_PWM_A,
		/* fclk_div3 666.67 MHz * 1250 ns = 833 counts */
		.pwm_total = 833, .pwm_clk_sel = 3,
		.pwm_en = BIT(0), .pwm_clk_en = BIT(15),
		.pwm_sel_shift = 4, .pwm_div_shift = 8,
		.opps = a73_opps, .nopps = ARRAY_SIZE(a73_opps),
		.first_cpu = 2, .last_cpu = 5,
		.dyn_khz = 1000000,	/* measured: fclk_div2, 998 MHz */
	},
	{
		.name = "A53", .mux_off = HHI_SYS_CPU_CLK_CNTL0,
		.pll_off = HHI_SYS1_PLL_CNTL0, .pwm_reg = REG_PWM_B,
		/* clk81 166.67 MHz * 1250 ns = 208 counts */
		.pwm_total = 208, .pwm_clk_sel = 1,
		.pwm_en = BIT(1), .pwm_clk_en = BIT(23),
		.pwm_sel_shift = 6, .pwm_div_shift = 16,
		.opps = a53_opps, .nopps = ARRAY_SIZE(a53_opps),
		.first_cpu = 0, .last_cpu = 1,
		/*
		 * 0 = do not use the dyn path.  Unlike the A73's, this
		 * cluster's dyn mux is parked on the 24 MHz xtal, not
		 * fclk_div2 - deselecting the PLL measured 19.3 MHz, not 1000.
		 * Every A53 rate therefore comes from sys1_pll.
		 */
		.dyn_khz = 0,
	},
};

static bool noop;
module_param(noop, bool, 0444);
MODULE_PARM_DESC(noop, "log transitions without touching any register");

static void __iomem *hhi_base;
static void __iomem *pwm_base;
static void __iomem *ao_base;
static struct cpufreq_frequency_table *freq_table;

static void rmw(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | (val & mask), reg);
}

/* Rounds to the nearest achievable duty; never exceeds the rail. */
static void g12b_set_voltage(struct g12b_cluster *c, unsigned int uv)
{
	unsigned int hi, lo;

	if (!c->pwm)			/* rail not described by firmware */
		return;
	if (uv < VMIN_UV)
		uv = VMIN_UV;
	if (uv > VMAX_UV)
		uv = VMAX_UV;

	/* duty = (VMAX - uv) / (VMAX - VMIN); hi is the high-time count */
	hi = DIV_ROUND_CLOSEST((VMAX_UV - uv) * c->pwm_total,
			       VMAX_UV - VMIN_UV);
	if (hi > c->pwm_total)
		hi = c->pwm_total;
	lo = c->pwm_total - hi;

	writel((hi << 16) | lo, c->pwm + c->pwm_reg);
}

/*
 * f = 24 MHz * m / 2^od with n = 1, m constrained to [128,250].  od=1 covers
 * 1536-3000 MHz and od=2 covers 768-1500, so both clusters' tables are
 * reachable - but only at exact multiples, which is why a few OPPs are omitted.
 */
static int pll_params(unsigned int khz, unsigned int *m_out, unsigned int *od_out)
{
	unsigned int od, m, step;

	for (od = 1; od <= 2; od++) {
		step = XTAL_KHZ >> od;
		m = khz / step;
		if (m >= PLL_M_MIN && m <= PLL_M_MAX && m * step == khz) {
			*m_out = m;
			*od_out = od;
			return 0;
		}
	}
	return -EINVAL;
}

static unsigned int g12b_get_freq_c(struct g12b_cluster *c)
{
	u32 pll;
	unsigned int m, n, od;

	if (!(readl(hhi_base + c->mux_off) & CLK_MUX_PLL)) {
		/*
		 * Clusters with no usable dyn path should never be here; if
		 * firmware left the mux that way, report the lowest OPP so
		 * CPUFREQ_NEED_INITIAL_FREQ_CHECK drives a real transition.
		 */
		return c->dyn_khz ? c->dyn_khz : c->opps[0].khz;
	}

	pll = readl(hhi_base + c->pll_off);
	m  = FIELD_GET(PLL_M_MASK, pll);
	n  = FIELD_GET(PLL_N_MASK, pll);
	od = FIELD_GET(PLL_OD_MASK, pll);
	if (!n)
		return 0;

	return (XTAL_KHZ * m / n) >> od;
}

static void mux_select(struct g12b_cluster *c, bool use_pll)
{
	rmw(hhi_base + c->mux_off, CLK_MUX_PLL, use_pll ? CLK_MUX_PLL : 0);
	/*
	 * Read back and settle.  writel() to Device memory orders the write
	 * against later writes, but says nothing about when the mux has
	 * PHYSICALLY switched.  Resetting the PLL while the cluster is still
	 * sourced from it removes the clock from the cores executing this
	 * code, which hangs the SoC hard.  This is the bug that killed the
	 * board twice: back-to-back park-then-reset from a kworker running on
	 * a core inside the cluster being reprogrammed.
	 */
	readl(hhi_base + c->mux_off);
	udelay(10);
}

/* One program-and-lock attempt.  Caller must already have parked the mux. */
static int pll_program_once(struct g12b_cluster *c, unsigned int m, unsigned int od)
{
	u32 v;
	int i;

	v = readl(hhi_base + c->pll_off);
	writel(v | PLL_RST, hhi_base + c->pll_off);
	writel((v | PLL_RST) & ~PLL_EN, hhi_base + c->pll_off);

	v = readl(hhi_base + c->pll_off);
	v &= ~(PLL_M_MASK | PLL_N_MASK | PLL_OD_MASK);
	v |= FIELD_PREP(PLL_M_MASK, m);
	v |= FIELD_PREP(PLL_N_MASK, 1);
	v |= FIELD_PREP(PLL_OD_MASK, od);
	writel(v, hhi_base + c->pll_off);

	writel(v | PLL_EN, hhi_base + c->pll_off);
	writel((v | PLL_EN) & ~PLL_RST, hhi_base + c->pll_off);

	/*
	 * 100 ms, matching meson_clk_pll_wait_lock() (5000 * 20 us).  2 ms was
	 * enough for every A73 rate but not for the A53.
	 */
	for (i = 0; i < 5000; i++) {
		if (readl(hhi_base + c->pll_off) & PLL_LOCK)
			return 0;
		udelay(20);
	}
	return -EIO;
}

static int g12b_pll_set(struct g12b_cluster *c, unsigned int khz)
{
	unsigned int m, od, prev = c->pll_khz;
	int ret, try;

	ret = pll_params(khz, &m, &od);
	if (ret)
		return ret;

	/*
	 * Fast path: the PLL is already at this rate, so just select it.  The
	 * ondemand governor oscillates between rates every few ms; without
	 * this we would reset and relock the PLL ~100 times a second.
	 */
	if (c->pll_khz == khz) {
		mux_select(c, true);
		return 0;
	}

	mux_select(c, false);		/* park before touching the PLL */

	/*
	 * Retry: locking is occasionally flaky on the A53, whose lowest rates
	 * sit just above the DCO minimum (m=128 => 3072 MHz; 1608 MHz needs
	 * m=134 => 3216).  A second attempt has always succeeded.
	 */
	for (try = 0; try < 3; try++) {
		ret = pll_program_once(c, m, od);
		if (!ret)
			break;
	}

	if (ret) {
		unsigned int pm, pod;

		pr_warn("g12b-cpufreq: %s PLL would not lock for %u kHz\n",
			c->name, khz);
		/*
		 * Do NOT leave the cluster parked on the dyn path.  For the
		 * A53 that is the 24 MHz xtal, so a failed transition would
		 * drop the little cores to a crawl.  Restore the rate that was
		 * running before.
		 */
		if (prev && !pll_params(prev, &pm, &pod) &&
		    !pll_program_once(c, pm, pod)) {
			c->pll_khz = prev;
			udelay(10);
			mux_select(c, true);
		} else {
			c->pll_khz = 0;		/* contents unknown */
		}
		return -EIO;
	}

	c->pll_khz = khz;
	udelay(10);			/* let the PLL output settle after lock */
	mux_select(c, true);
	return 0;
}

static int g12b_set_freq(struct g12b_cluster *c, unsigned int khz)
{
	if (c->dyn_khz && khz <= c->dyn_khz) {
		/* fclk_div2 = 1000 MHz; just deselect the PLL. */
		mux_select(c, false);
		return 0;
	}
	return g12b_pll_set(c, khz);
}

static int g12b_target_index(struct cpufreq_policy *policy, unsigned int index)
{
	struct g12b_cluster *c = policy->driver_data;
	unsigned int new_khz = c->ft[index].frequency;
	unsigned int old_khz = policy->cur;
	unsigned int uv = c->opps[index].uv;
	int ret;

	if (noop) {		/* BISECT: exercise cpufreq/governor path only */
		pr_info_ratelimited("g12b-cpufreq: %s NOOP %u -> %u kHz (uv %u)\n",
				    c->name, old_khz, new_khz, uv);
		return 0;
	}

	/* Raise volts before frequency; drop frequency before volts. */
	if (new_khz > old_khz) {
		g12b_set_voltage(c, uv);
		udelay(200);
		ret = g12b_set_freq(c, new_khz);
		if (ret) {	/* clock never moved - put the volts back */
			int i = cpufreq_table_find_index_h(policy, old_khz, false);

			if (i >= 0)
				g12b_set_voltage(c, c->opps[i].uv);
		}
	} else {
		ret = g12b_set_freq(c, new_khz);
		if (!ret) {
			udelay(200);
			g12b_set_voltage(c, uv);
		}
	}

	return ret;
}

static unsigned int g12b_get_freq(unsigned int cpu)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clusters); i++)
		if (cpu >= clusters[i].first_cpu && cpu <= clusters[i].last_cpu)
			return g12b_get_freq_c(&clusters[i]);
	return 0;
}

static int g12b_cpufreq_init(struct cpufreq_policy *policy)
{
	struct g12b_cluster *c = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(clusters); i++)
		if (policy->cpu >= clusters[i].first_cpu &&
		    policy->cpu <= clusters[i].last_cpu)
			c = &clusters[i];

	/* No table means firmware did not describe this cluster's rail. */
	if (!c || !c->ft)
		return -ENODEV;

	for (i = c->first_cpu; i <= c->last_cpu; i++) {
		cpumask_set_cpu(i, policy->cpus);
		/*
		 * Publish the cluster's top frequency as the scheduler's
		 * capacity reference.  Nothing else will: that normally comes
		 * from DT capacity-dmips-mhz or CPPC, and ACPI-without-CPPC has
		 * neither, so arch_scale_freq_ref() returns 0 - which both
		 * trips WARN_ON_ONCE in topology_set_freq_scale() and leaves
		 * frequency-invariant load tracking disabled.  That matters
		 * here: without it the scheduler cannot compare utilisation
		 * across the A53 and A73 clusters.
		 */
		per_cpu(capacity_freq_ref, i) = c->opps[c->nopps - 1].khz;
	}

	policy->driver_data = c;
	policy->freq_table = c->ft;
	policy->cpuinfo.transition_latency = 2 * NSEC_PER_MSEC;
	policy->cur = g12b_get_freq_c(c);

	return 0;
}

/*
 * Runs after cpufreq_online() has validated and sorted the frequency table.
 * The cooling device cannot be registered from .init: __cpufreq_cooling_register()
 * rejects policy->freq_table_sorted == CPUFREQ_TABLE_UNSORTED, and the core only
 * sets that field in cpufreq_table_validate_and_sort(), which runs after .init
 * returns ("unsorted frequency tables are not supported", -EINVAL).
 */
static void g12b_cpufreq_ready(struct cpufreq_policy *policy)
{
	struct g12b_cluster *c = policy->driver_data;

	/*
	 * Register it here rather than setting CPUFREQ_IS_COOLING_DEV: that
	 * flag makes the core call of_cpufreq_cooling_register(), which opens
	 * with of_get_cpu_node(policy->cpu) and returns NULL under ACPI - the
	 * "cpufreq_cooling: OF node not available for cpuN" message.  The
	 * result was no cooling device at all, so the 85 C passive trip had
	 * nothing to act on and the only protection left was the 110 C
	 * critical trip: shutdown rather than throttle.
	 *
	 * Binding it to the trip is the thermal driver's job - see its
	 * should_bind callback.
	 */
	policy->cdev = cpufreq_cooling_register(policy);
	if (IS_ERR(policy->cdev)) {
		pr_warn("g12b-cpufreq: %s cooling device failed: %ld\n",
			c->name, PTR_ERR(policy->cdev));
		policy->cdev = NULL;
	} else {
		pr_info("g12b-cpufreq: %s cooling device registered\n", c->name);
	}
}

static void g12b_cpufreq_exit(struct cpufreq_policy *policy)
{
	if (policy->cdev) {
		cpufreq_cooling_unregister(policy->cdev);
		policy->cdev = NULL;
	}
}

static struct cpufreq_driver g12b_cpufreq_driver = {
	.name		= "g12b-acpi",
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= g12b_target_index,
	.get		= g12b_get_freq,
	.init		= g12b_cpufreq_init,
	.ready		= g12b_cpufreq_ready,
	.exit		= g12b_cpufreq_exit,
};

static int g12b_setup_cluster(struct device *dev, struct g12b_cluster *c,
			      void __iomem *pwm)
{
	int i;

	c->pwm = pwm;
	if (pwm) {
		/*
		 * Program the period rather than inheriting it.  What firmware
		 * leaves is neither the configuration the rails were
		 * characterised at nor stable across boots (VDDCPU_B has been
		 * seen with both 88 and 28 counts).  These settings reproduce
		 * the 1250 ns the DT pwm-regulator uses, which is what the
		 * duty->voltage mapping was measured against.
		 */
		if (!noop)
			rmw(pwm + REG_MISC_AB,
			    c->pwm_en | c->pwm_clk_en |
			    (0x3u << c->pwm_sel_shift) |
			    (0x7fu << c->pwm_div_shift),
			    c->pwm_en | c->pwm_clk_en |
			    (c->pwm_clk_sel << c->pwm_sel_shift));
	} else {
		dev_info(dev, "%s: no rail described, cluster not managed\n",
			 c->name);
		return 0;
	}

	c->ft = devm_kcalloc(dev, c->nopps + 1, sizeof(*c->ft), GFP_KERNEL);
	if (!c->ft)
		return -ENOMEM;
	for (i = 0; i < c->nopps; i++) {
		c->ft[i].frequency = c->opps[i].khz;
		c->ft[i].driver_data = i;
	}
	c->ft[i].frequency = CPUFREQ_TABLE_END;

	/*
	 * Start at the HIGHEST voltage: the cluster may already be running at
	 * any rate, and dropping the rail below what the running frequency
	 * needs corrupts silently rather than failing loudly.  The first
	 * governor transition brings it down to the correct value.
	 *
	 * The A53 in particular boots undervolted - firmware leaves it at
	 * ~723 mV while running 1200 MHz, an OPP that wants 781 mV.
	 */
	if (!noop)
		g12b_set_voltage(c, c->opps[c->nopps - 1].uv);

	c->pll_khz = (readl(hhi_base + c->mux_off) & CLK_MUX_PLL) ?
		     g12b_get_freq_c(c) : 0;

	dev_info(dev, "%s: %u OPPs %u-%u kHz, PWM period %u, running %u kHz\n",
		 c->name, c->nopps, c->opps[0].khz, c->opps[c->nopps - 1].khz,
		 c->pwm_total, g12b_get_freq_c(c));
	return 0;
}

static int g12b_cpufreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *pwm_ab, *pwm_ao;
	struct resource *res;
	int i, ret;

	/* Resource order is fixed by the DSDT _CRS; see Dsdt.asl CPUF. */
	hhi_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hhi_base))
		return dev_err_probe(dev, PTR_ERR(hhi_base), "HHI\n");
	pwm_ab = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(pwm_ab))
		return dev_err_probe(dev, PTR_ERR(pwm_ab), "PWM_AB\n");
	ao_base = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(ao_base))
		return dev_err_probe(dev, PTR_ERR(ao_base), "AO pinmux\n");

	/*
	 * Resource 3 (PWM_AO_CD, the A53 rail) is OPTIONAL: firmware that
	 * predates it still drives the A73 cluster correctly.
	 */
	pwm_ao = NULL;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	if (res) {
		pwm_ao = devm_ioremap_resource(dev, res);
		if (IS_ERR(pwm_ao))
			return dev_err_probe(dev, PTR_ERR(pwm_ao), "PWM_AO_CD\n");
	}

	/*
	 * A73 rail only: enable the PWM channel and mux its pin.  Order
	 * matters - muxing first would briefly expose the rail to the disabled
	 * block's static output, and a stuck-high pin is 100% duty i.e. VMIN.
	 * The A53 rail is already enabled and muxed by firmware (GPIOE_1), so
	 * it needs neither.
	 */
	if (!noop) {
		rmw(ao_base, 0xfu << GPIOE_2_SHIFT,
		    GPIOE_2_PWM_A_E << GPIOE_2_SHIFT);
		udelay(500);
	}

	for (i = 0; i < ARRAY_SIZE(clusters); i++) {
		ret = g12b_setup_cluster(dev, &clusters[i],
					 clusters[i].first_cpu >= 2 ? pwm_ab
								    : pwm_ao);
		if (ret)
			return ret;
	}

	ret = cpufreq_register_driver(&g12b_cpufreq_driver);
	if (ret)
		return dev_err_probe(dev, ret, "cpufreq_register_driver\n");

	return 0;
}

static void g12b_cpufreq_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&g12b_cpufreq_driver);
}

static const struct of_device_id g12b_cpufreq_match[] = {
	{ .compatible = "amlogic,g12b-cpufreq" },
	{ }
};
MODULE_DEVICE_TABLE(of, g12b_cpufreq_match);

static struct platform_driver g12b_cpufreq_platform_driver = {
	.driver = {
		.name		= "g12b-cpufreq",
		.of_match_table	= g12b_cpufreq_match,
	},
	.probe	= g12b_cpufreq_probe,
	.remove	= g12b_cpufreq_remove,
};
module_platform_driver(g12b_cpufreq_platform_driver);

MODULE_DESCRIPTION("Amlogic G12B cpufreq for ACPI-mode boots");
MODULE_LICENSE("GPL");
