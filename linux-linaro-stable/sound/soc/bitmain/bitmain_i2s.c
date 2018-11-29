/*
 * I2S driver on bitmain BM1880
 *
 * Copyright 2018 Bitmain
 *
 * Author: EthanChen
 *
 * Licensed under the GPL-2 or later.
 */


#include <linux/clk.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <sound/designware_i2s.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>
#include "local.h"
#include <soc/bitmain/subsys_int.h>

static inline void i2s_write_reg(void __iomem *io_base, int reg, u32 val)
{
	writel(val, io_base + reg);
}

static inline u32 i2s_read_reg(void __iomem *io_base, int reg)
{
	return readl(io_base + reg);
}

static inline void i2s_disable_channels(struct dw_i2s_dev *dev, u32 stream)
{
	u32 i = 0;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		for (i = 0; i < 4; i++)
			i2s_write_reg(dev->i2s_base, TER(i), 0);
	} else {
		for (i = 0; i < 4; i++)
			i2s_write_reg(dev->i2s_base, RER(i), 0);
	}
}

static inline void i2s_clear_irqs(struct dw_i2s_dev *dev, u32 stream)
{
	u32 i = 0;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		for (i = 0; i < 4; i++)
			i2s_read_reg(dev->i2s_base, TOR(i));
	} else {
		for (i = 0; i < 4; i++)
			i2s_read_reg(dev->i2s_base, ROR(i));
	}
}

static inline void i2s_disable_irqs(struct dw_i2s_dev *dev, u32 stream,
				    int chan_nr)
{
	u32 i, irq;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		for (i = 0; i < (chan_nr / 2); i++) {
			irq = i2s_read_reg(dev->i2s_base, IMR(i));
			i2s_write_reg(dev->i2s_base, IMR(i), irq | 0x30);
		}
	} else {
		for (i = 0; i < (chan_nr / 2); i++) {
			irq = i2s_read_reg(dev->i2s_base, IMR(i));
			i2s_write_reg(dev->i2s_base, IMR(i), irq | 0x03);
		}
	}
}

static inline void i2s_enable_irqs(struct dw_i2s_dev *dev, u32 stream,
				   int chan_nr)
{
	u32 i, irq;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		for (i = 0; i < (chan_nr / 2); i++) {
			irq = i2s_read_reg(dev->i2s_base, IMR(i));
			i2s_write_reg(dev->i2s_base, IMR(i), irq & ~0x30);
		}
	} else {
		for (i = 0; i < (chan_nr / 2); i++) {
			irq = i2s_read_reg(dev->i2s_base, IMR(i));
			i2s_write_reg(dev->i2s_base, IMR(i), irq & ~0x03);
		}
	}
}

static irqreturn_t i2s_irq_handler(int irq, void *dev_id)
{
	struct dw_i2s_dev *dev = dev_id;
	bool irq_valid = false;
	u32 isr[4];
	int i;

	for (i = 0; i < 4; i++)
		isr[i] = i2s_read_reg(dev->i2s_base, ISR(i));

	i2s_clear_irqs(dev, SNDRV_PCM_STREAM_PLAYBACK);
	i2s_clear_irqs(dev, SNDRV_PCM_STREAM_CAPTURE);

	for (i = 0; i < 4; i++) {
		/*
		 * Check if TX fifo is empty. If empty fill FIFO with samples
		 * NOTE: Only two channels supported
		 */
		if ((isr[i] & ISR_TXFE) && (i == 0) && dev->use_pio) {
			dw_pcm_push_tx(dev);
			irq_valid = true;
		}

		/* Data available. Record mode not supported in PIO mode */
		if (isr[i] & ISR_RXDA)
			irq_valid = true;

		/* Error Handling: TX */
		if (isr[i] & ISR_TXFO) {
			dev_err(dev->dev, "TX overrun (ch_id=%d)\n", i);
			irq_valid = true;
		}

		/* Error Handling: TX */
		if (isr[i] & ISR_RXFO) {
			dev_err(dev->dev, "RX overrun (ch_id=%d)\n", i);
			irq_valid = true;
		}
	}

	if (irq_valid)
		return IRQ_HANDLED;
	else
		return IRQ_NONE;
}

static void i2s_start(struct dw_i2s_dev *dev,
		      struct snd_pcm_substream *substream)
{
	struct i2s_clk_config_data *config = &dev->config;
	u32 ctl_val;

	i2s_write_reg(dev->i2s_base, IER, 1);

#if defined(CONFIG_ARCH_BM1880)
	ctl_val = i2s_read_reg(dev->i2s_base, I2S_USER_REG_CTL);
	i2s_write_reg(dev->i2s_base, I2S_USER_REG_CTL, ctl_val|0x0001);
#endif

	i2s_enable_irqs(dev, substream->stream, config->chan_nr);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		i2s_write_reg(dev->i2s_base, ITER, 1);
	else
		i2s_write_reg(dev->i2s_base, IRER, 1);

#if !defined(CONFIG_ARCH_BM1880)
/* i2s in bm1880 use externel clock generator */
	i2s_write_reg(dev->i2s_base, CER, 1);
#endif
}

static void i2s_stop(struct dw_i2s_dev *dev,
		struct snd_pcm_substream *substream)
{
	u32 ctl_val;

	i2s_clear_irqs(dev, substream->stream);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		i2s_write_reg(dev->i2s_base, ITER, 0);
	else
		i2s_write_reg(dev->i2s_base, IRER, 0);

	i2s_disable_irqs(dev, substream->stream, 8);

	if (!dev->active) {
#if !defined(CONFIG_ARCH_BM1880)
		i2s_write_reg(dev->i2s_base, CER, 0);
#endif
		i2s_write_reg(dev->i2s_base, IER, 0);

		ctl_val = i2s_read_reg(dev->i2s_base, I2S_USER_REG_CTL);
		ctl_val &= ~0x0001;
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_CTL, ctl_val);
	}
}

static int dw_i2s_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *cpu_dai)
{
	struct dw_i2s_dev *dev = snd_soc_dai_get_drvdata(cpu_dai);
	union dw_i2s_snd_dma_data *dma_data = NULL;

	if (!(dev->capability & DWC_I2S_RECORD) &&
			(substream->stream == SNDRV_PCM_STREAM_CAPTURE))
		return -EINVAL;

	if (!(dev->capability & DWC_I2S_PLAY) &&
			(substream->stream == SNDRV_PCM_STREAM_PLAYBACK))
		return -EINVAL;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dma_data = &dev->play_dma_data;
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		dma_data = &dev->capture_dma_data;

	snd_soc_dai_set_dma_data(cpu_dai, substream, (void *)dma_data);

	return 0;
}

static void dw_i2s_config(struct dw_i2s_dev *dev, int stream)
{
	u32 ch_reg;
	struct i2s_clk_config_data *config = &dev->config;

	i2s_disable_channels(dev, stream);

	for (ch_reg = 0; ch_reg < (config->chan_nr / 2); ch_reg++) {
		if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
			i2s_write_reg(dev->i2s_base, TCR(ch_reg),
				      dev->xfer_resolution);
			i2s_write_reg(dev->i2s_base, TFCR(ch_reg),
				      dev->fifo_th - 1);
			i2s_write_reg(dev->i2s_base, TER(ch_reg), 1);
		} else {
			i2s_write_reg(dev->i2s_base, RCR(ch_reg),
				      dev->xfer_resolution);
			i2s_write_reg(dev->i2s_base, RFCR(ch_reg),
				      dev->fifo_th - 1);
			i2s_write_reg(dev->i2s_base, RER(ch_reg), 1);
		}

	}

#if defined(CONFIG_ARCH_BM1880)
	/* Configure I2S user sync_div here*/
	switch (dev->xfer_resolution) {
	case (WLEN_32_BIT):
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_FRM, WSS_32_CLKCYCLE);
		break;
	case (WLEN_24_BIT):
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_FRM, WSS_32_CLKCYCLE);
		break;
	case (WLEN_16_BIT):
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_FRM, WSS_16_CLKCYCLE);
		break;
	default:
		dev_err(dev->dev, "resolution not supported\n");
	}
#endif

}

static int dw_i2s_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct dw_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);
	struct i2s_clk_config_data *config = &dev->config;
	int ret;
#if defined(CONFIG_ARCH_BM1880)
	u32 ctl_val = i2s_read_reg(dev->i2s_base, I2S_USER_REG_CTL) & (~SAMPLEBIT_MASK);
	u32 clk_val = i2s_read_reg(dev->i2s_base, I2S_USER_REG_CLK) & (~I2S_BCLK_MASK);
	u32 temp_bclk_div = 0;
#endif

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		config->data_width = 16;
		dev->ccr = 0x00;
		dev->xfer_resolution = 0x02;
#if defined(CONFIG_ARCH_BM1880)
		ctl_val |= SAMPLEBIT_16_CLKCYCLE;
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_CTL, ctl_val);
#endif
		break;

	case SNDRV_PCM_FORMAT_S24_LE:
		config->data_width = 24;
		dev->ccr = 0x08;
		dev->xfer_resolution = 0x04;
#if defined(CONFIG_ARCH_BM1880)
		ctl_val |= SAMPLEBIT_24_CLKCYCLE;
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_CTL, ctl_val);
#endif
		break;

	case SNDRV_PCM_FORMAT_S32_LE:
		config->data_width = 32;
		dev->ccr = 0x10;
		dev->xfer_resolution = 0x05;
#if defined(CONFIG_ARCH_BM1880)
		ctl_val |= SAMPLEBIT_32_CLKCYCLE;
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_CTL, ctl_val);
#endif
		break;

	default:
		dev_err(dev->dev, "designware-i2s: unsupported PCM fmt");
		return -EINVAL;
	}

	config->chan_nr = params_channels(params);

	switch (config->chan_nr) {
	case EIGHT_CHANNEL_SUPPORT:
	case SIX_CHANNEL_SUPPORT:
	case FOUR_CHANNEL_SUPPORT:
	case TWO_CHANNEL_SUPPORT:
		break;
	default:
		dev_err(dev->dev, "channel not supported\n");
		return -EINVAL;
	}

	dw_i2s_config(dev, substream->stream);

	i2s_write_reg(dev->i2s_base, CCR, dev->ccr);

	config->sample_rate = params_rate(params);

#if defined(CONFIG_ARCH_BM1880)
	/* Configure I2S user bclk_div and sync_div here*/
	switch (dev->xfer_resolution) {
	case (WLEN_32_BIT):
		temp_bclk_div = (clk_get_rate(dev->clk) / 1000) / (WSS_32_CLKCYCLE * (config->sample_rate / 1000));
		if (temp_bclk_div == 0x01)
			temp_bclk_div = 0;
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_CLK, clk_val | temp_bclk_div);
		break;
	case (WLEN_24_BIT):
		temp_bclk_div = (clk_get_rate(dev->clk) / 1000) / (WSS_32_CLKCYCLE * (config->sample_rate / 1000));
		if (temp_bclk_div == 0x01)
			temp_bclk_div = 0;
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_CLK, clk_val | temp_bclk_div);
		break;
	case (WLEN_16_BIT):
		temp_bclk_div = (clk_get_rate(dev->clk) / 1000) / (WSS_16_CLKCYCLE * (config->sample_rate / 1000));
		if (temp_bclk_div == 0x01)
			temp_bclk_div = 0;
		i2s_write_reg(dev->i2s_base, I2S_USER_REG_CLK, clk_val | temp_bclk_div);
		break;
	default:
		dev_err(dev->dev, "resolution not supported\n");
	}

#endif

	if (dev->capability & DW_I2S_MASTER) {
		if (dev->i2s_clk_cfg) {
			ret = dev->i2s_clk_cfg(config);
			if (ret < 0) {
				dev_err(dev->dev, "runtime audio clk config fail\n");
				return ret;
			}
		} else {
			u32 bitclk = config->sample_rate *
					config->data_width * 2;

			ret = clk_set_rate(dev->clk, bitclk);
			if (ret) {
				dev_err(dev->dev, "Can't set I2S clock rate: %d\n",
					ret);
				return ret;
			}
		}
	}
	return 0;
}

static void dw_i2s_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{

	snd_soc_dai_set_dma_data(dai, substream, NULL);
}

static int dw_i2s_prepare(struct snd_pcm_substream *substream,
			  struct snd_soc_dai *dai)
{
	struct dw_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		i2s_write_reg(dev->i2s_base, TXFFR, 1);
	else
		i2s_write_reg(dev->i2s_base, RXFFR, 1);

	return 0;
}

static int dw_i2s_trigger(struct snd_pcm_substream *substream,
		int cmd, struct snd_soc_dai *dai)
{
	struct dw_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		dev->active++;
		i2s_start(dev, substream);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		dev->active--;
		i2s_stop(dev, substream);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int dw_i2s_set_fmt(struct snd_soc_dai *cpu_dai, unsigned int fmt)
{
	struct dw_i2s_dev *dev = snd_soc_dai_get_drvdata(cpu_dai);
#if defined(CONFIG_ARCH_BM1880)
	u32 ctl_val = i2s_read_reg(dev->i2s_base, I2S_USER_REG_CTL);
#endif
	int ret = 0;

#if defined(CONFIG_ARCH_BM1880)
	ctl_val &= ~DW_USER_I2S_MASTER_MASK;
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		ctl_val |= DW_USER_I2S_MASTER;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		ctl_val |= DW_USER_I2S_SLAVE;
		break;
	case SND_SOC_DAIFMT_CBM_CFS:
	case SND_SOC_DAIFMT_CBS_CFM:
		ret = -EINVAL;
		break;
	default:
		dev_dbg(dev->dev, "dwc : Invalid master/slave format\n");
		ret = -EINVAL;
		break;
	}

	ctl_val &= ~DW_I2S_FMT_INV_MASK;
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		ctl_val |= DW_I2S_FMT_NB_NF;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		ctl_val |= DW_I2S_FMT_NB_IF;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		ctl_val |= DW_I2S_FMT_IB_NF;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		ctl_val |= DW_I2S_FMT_IB_IF;
		break;
	default:
		dev_dbg(dev->dev, "dwc : Invalid frame format\n");
		ret = -EINVAL;
		break;
	}

	ctl_val &= ~DW_I2S_FMT_MASK;
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		ctl_val |= DW_I2S_FMT_I2S_MODE;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		ctl_val |= DW_I2S_FMT_LJ_MODE;
		break;
	default:
		dev_dbg(dev->dev, "dwc : Invalid I2S mode\n");
		ret = -EINVAL;
		break;
	}
#else
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		if (dev->capability & DW_I2S_SLAVE)
			ret = 0;
		else
			ret = -EINVAL;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		if (dev->capability & DW_I2S_MASTER)
			ret = 0;
		else
			ret = -EINVAL;
		break;
	case SND_SOC_DAIFMT_CBM_CFS:
	case SND_SOC_DAIFMT_CBS_CFM:
		ret = -EINVAL;
		break;
	default:
		dev_dbg(dev->dev, "dwc : Invalid master/slave format\n");
		ret = -EINVAL;
		break;
	}
#endif
	return ret;
}

static struct snd_soc_dai_ops dw_i2s_dai_ops = {
	.startup	= dw_i2s_startup,
	.shutdown	= dw_i2s_shutdown,
	.hw_params	= dw_i2s_hw_params,
	.prepare	= dw_i2s_prepare,
	.trigger	= dw_i2s_trigger,
	.set_fmt	= dw_i2s_set_fmt,
};

static const struct snd_soc_component_driver dw_i2s_component = {
	.name		= "dw-i2s",
};

#ifdef CONFIG_PM
static int dw_i2s_runtime_suspend(struct device *dev)
{
	struct dw_i2s_dev *dw_dev = dev_get_drvdata(dev);
#if defined(CONFIG_ARCH_BM1880)
	u32 ctl_val = i2s_read_reg(dw_dev->i2s_base, I2S_USER_REG_CTL);

	i2s_write_reg(dw_dev->i2s_base, I2S_USER_REG_CTL, ctl_val & ~I2S_USER_AU_EN);
#endif

	if (dw_dev->capability & DW_I2S_MASTER)
		clk_disable(dw_dev->clk);
	return 0;
}

static int dw_i2s_runtime_resume(struct device *dev)
{
	struct dw_i2s_dev *dw_dev = dev_get_drvdata(dev);
#if defined(CONFIG_ARCH_BM1880)
	u32 ctl_val = i2s_read_reg(dw_dev->i2s_base, I2S_USER_REG_CTL);

	i2s_write_reg(dw_dev->i2s_base, I2S_USER_REG_CTL, ctl_val | I2S_USER_AU_EN);
#endif
	if (dw_dev->capability & DW_I2S_MASTER)
		clk_enable(dw_dev->clk);
	return 0;
}

static int dw_i2s_suspend(struct snd_soc_dai *dai)
{
	struct dw_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);
#if defined(CONFIG_ARCH_BM1880)
	u32 ctl_val = i2s_read_reg(dev->i2s_base, I2S_USER_REG_CTL);

	i2s_write_reg(dev->i2s_base, I2S_USER_REG_CTL, ctl_val & ~I2S_USER_AU_EN);
#endif

	if (dev->capability & DW_I2S_MASTER)
		clk_disable(dev->clk);
	return 0;
}

static int dw_i2s_resume(struct snd_soc_dai *dai)
{
	struct dw_i2s_dev *dev = snd_soc_dai_get_drvdata(dai);
#if defined(CONFIG_ARCH_BM1880)
	u32 ctl_val = i2s_read_reg(dev->i2s_base, I2S_USER_REG_CTL);

	i2s_write_reg(dev->i2s_base, I2S_USER_REG_CTL, ctl_val | I2S_USER_AU_EN);
#endif

	if (dev->capability & DW_I2S_MASTER)
		clk_enable(dev->clk);

	if (dai->playback_active)
		dw_i2s_config(dev, SNDRV_PCM_STREAM_PLAYBACK);
	if (dai->capture_active)
		dw_i2s_config(dev, SNDRV_PCM_STREAM_CAPTURE);
	return 0;
}

#else
#define dw_i2s_suspend	NULL
#define dw_i2s_resume	NULL
#endif

/*
 * The following tables allow a direct lookup of various parameters
 * defined in the I2S block's configuration in terms of sound system
 * parameters.  Each table is sized to the number of entries possible
 * according to the number of configuration bits describing an I2S
 * block parameter.
 */

/* Maximum bit resolution of a channel - not uniformly spaced */
static const u32 fifo_width[COMP_MAX_WORDSIZE] = {
	12, 16, 20, 24, 32, 0, 0, 0
};

/* Width of (DMA) bus */
static const u32 bus_widths[COMP_MAX_DATA_WIDTH] = {
	DMA_SLAVE_BUSWIDTH_1_BYTE,
	DMA_SLAVE_BUSWIDTH_2_BYTES,
	DMA_SLAVE_BUSWIDTH_4_BYTES,
	DMA_SLAVE_BUSWIDTH_UNDEFINED
};

/* PCM format to support channel resolution */
static const u32 formats[COMP_MAX_WORDSIZE] = {
	SNDRV_PCM_FMTBIT_S16_LE,
	SNDRV_PCM_FMTBIT_S16_LE,
	SNDRV_PCM_FMTBIT_S24_LE,
	SNDRV_PCM_FMTBIT_S24_LE,
	SNDRV_PCM_FMTBIT_S32_LE,
	0,
	0,
	0
};

static int dw_configure_dai(struct dw_i2s_dev *dev,
				   struct snd_soc_dai_driver *dw_i2s_dai,
				   unsigned int rates)
{
	/*
	 * Read component parameter registers to extract
	 * the I2S block's configuration.
	 */
	u32 comp1 = i2s_read_reg(dev->i2s_base, dev->i2s_reg_comp1);
	u32 comp2 = i2s_read_reg(dev->i2s_base, dev->i2s_reg_comp2);
	u32 fifo_depth = 1 << (1 + COMP1_FIFO_DEPTH_GLOBAL(comp1));
	u32 idx;

	dev_info(dev->dev, "%s, comp1=0x%X, comp2=0x%X, capability=0x%X, quirks=0x%X\n", __func__, comp1, comp2,
	dev->capability, dev->quirks);

	if (dev->capability & DWC_I2S_RECORD &&
			dev->quirks & DW_I2S_QUIRK_COMP_PARAM1)
		comp1 = comp1 & ~BIT(5);

	if (COMP1_TX_ENABLED(comp1)) {
		dev_info(dev->dev, " designware: play supported\n");
		idx = COMP1_TX_WORDSIZE_0(comp1);
		if (WARN_ON(idx >= ARRAY_SIZE(formats)))
			return -EINVAL;
		dw_i2s_dai->playback.channels_min = MIN_CHANNEL_NUM;
		dw_i2s_dai->playback.channels_max =
				1 << (COMP1_TX_CHANNELS(comp1) + 1);
		//dw_i2s_dai->playback.formats = formats[idx];
		dw_i2s_dai->playback.formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S16_LE;
		dw_i2s_dai->playback.rates = rates;
	}

	if (COMP1_RX_ENABLED(comp1)) {
		dev_info(dev->dev, "designware: record supported\n");
		idx = COMP2_RX_WORDSIZE_0(comp2);
		if (WARN_ON(idx >= ARRAY_SIZE(formats)))
			return -EINVAL;
		dw_i2s_dai->capture.channels_min = MIN_CHANNEL_NUM;
		dw_i2s_dai->capture.channels_max =
				1 << (COMP1_RX_CHANNELS(comp1) + 1);
		//dw_i2s_dai->capture.formats = formats[idx];
		dw_i2s_dai->capture.formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S16_LE;
		dw_i2s_dai->capture.rates = rates;
	}

	if (COMP1_MODE_EN(comp1)) {
		dev_info(dev->dev, "designware: i2s master mode supported\n");
		dev->capability |= DW_I2S_MASTER;
	} else {
		dev_info(dev->dev, "designware: i2s slave mode supported\n");
		dev->capability |= DW_I2S_SLAVE;
	}

	dev->fifo_th = fifo_depth / 2;
	return 0;
}

static int dw_configure_dai_by_pd(struct dw_i2s_dev *dev,
				   struct snd_soc_dai_driver *dw_i2s_dai,
				   struct resource *res,
				   const struct i2s_platform_data *pdata)
{
	u32 comp1 = i2s_read_reg(dev->i2s_base, dev->i2s_reg_comp1);
	u32 idx = COMP1_APB_DATA_WIDTH(comp1);
	int ret;

	dev_info(dev->dev, "%s\n", __func__);

	if (WARN_ON(idx >= ARRAY_SIZE(bus_widths)))
		return -EINVAL;

	ret = dw_configure_dai(dev, dw_i2s_dai, pdata->snd_rates);
	if (ret < 0)
		return ret;

	/* Set DMA slaves info */
	dev->play_dma_data.pd.data = pdata->play_dma_data;
	dev->capture_dma_data.pd.data = pdata->capture_dma_data;
	dev->play_dma_data.pd.addr = res->start + I2S_TXDMA;
	dev->capture_dma_data.pd.addr = res->start + I2S_RXDMA;
	dev->play_dma_data.pd.max_burst = 16;
	dev->capture_dma_data.pd.max_burst = 16;
	dev->play_dma_data.pd.addr_width = bus_widths[idx];
	dev->capture_dma_data.pd.addr_width = bus_widths[idx];
	dev->play_dma_data.pd.filter = pdata->filter;
	dev->capture_dma_data.pd.filter = pdata->filter;

	return 0;
}

static int dw_configure_dai_by_dt(struct dw_i2s_dev *dev,
				   struct snd_soc_dai_driver *dw_i2s_dai,
				   struct resource *res)
{
	u32 comp1 = i2s_read_reg(dev->i2s_base, I2S_COMP_PARAM_1);
	u32 comp2 = i2s_read_reg(dev->i2s_base, I2S_COMP_PARAM_2);
	u32 fifo_depth = 1 << (1 + COMP1_FIFO_DEPTH_GLOBAL(comp1));
	u32 idx = COMP1_APB_DATA_WIDTH(comp1);
	u32 idx2;
	int ret;
	u32 user_reg = i2s_read_reg(dev->i2s_base, I2S_USER_REG_CTL);

	dev_info(dev->dev, "%s, user_reg=0x%X, capability=0x%X\n", __func__, user_reg, dev->capability);

	if (WARN_ON(idx >= ARRAY_SIZE(bus_widths)))
		return -EINVAL;

	ret = dw_configure_dai(dev, dw_i2s_dai, SNDRV_PCM_RATE_8000_192000);
	if (ret < 0)
		return ret;

	if (COMP1_TX_ENABLED(comp1)) {
		idx2 = COMP1_TX_WORDSIZE_0(comp1);

		dev->capability |= DWC_I2S_PLAY;
		dev->play_dma_data.dt.addr = res->start + I2S_TXDMA;
		dev->play_dma_data.dt.addr_width = bus_widths[idx];
		dev->play_dma_data.dt.fifo_size = fifo_depth *
			(fifo_width[idx2]) >> 8;
		dev->play_dma_data.dt.maxburst = 16;
	}
	if (COMP1_RX_ENABLED(comp1)) {

		idx2 = COMP2_RX_WORDSIZE_0(comp2);

		dev->capability |= DWC_I2S_RECORD;
		dev->capture_dma_data.dt.addr = res->start + I2S_RXDMA;
		dev->capture_dma_data.dt.addr_width = bus_widths[idx];
		dev->capture_dma_data.dt.fifo_size = fifo_depth *
			(fifo_width[idx2] >> 8);
		dev->capture_dma_data.dt.maxburst = 16;
	}

	return 0;

}

static int dw_i2s_probe(struct platform_device *pdev)
{
	const struct i2s_platform_data *pdata = pdev->dev.platform_data;
	struct dw_i2s_dev *dev;
	struct resource *res;
	int ret, irq;
	struct snd_soc_dai_driver *dw_i2s_dai;
	const char *clk_id;
	int port;
	struct device_node *np = pdev->dev.of_node;

	dev_info(&pdev->dev, "%s\n", __func__);

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dw_i2s_dai = devm_kzalloc(&pdev->dev, sizeof(*dw_i2s_dai), GFP_KERNEL);
	if (!dw_i2s_dai)
		return -ENOMEM;

	dw_i2s_dai->ops = &dw_i2s_dai_ops;
	dev_info(&pdev->dev, "%s, trigger=%p\n", __func__, dw_i2s_dai->ops->trigger);
	dw_i2s_dai->suspend = dw_i2s_suspend;
	dw_i2s_dai->resume = dw_i2s_resume;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dev->i2s_base = devm_ioremap_resource(&pdev->dev, res);
	dev_info(&pdev->dev, "I2S get i2s_base=0x%p\n", dev->i2s_base);
	if (IS_ERR(dev->i2s_base))
		return PTR_ERR(dev->i2s_base);

	dev->dev = &pdev->dev;

	irq = platform_get_irq(pdev, 0);
	if (irq >= 0) {
		dev_info(&pdev->dev, "I2S get IRQ=0x%x\n", irq);
		ret = devm_request_irq(&pdev->dev, irq, i2s_irq_handler, 0,
				pdev->name, dev);
		if (ret < 0) {
			dev_err(&pdev->dev, "failed to request irq\n");
			return ret;
		}
	}

	dev->i2s_reg_comp1 = I2S_COMP_PARAM_1;
	dev->i2s_reg_comp2 = I2S_COMP_PARAM_2;
	if (pdata) {
		dev->capability = pdata->cap;
		clk_id = NULL;
		dev->quirks = pdata->quirks;
		if (dev->quirks & DW_I2S_QUIRK_COMP_REG_OFFSET) {
			dev->i2s_reg_comp1 = pdata->i2s_reg_comp1;
			dev->i2s_reg_comp2 = pdata->i2s_reg_comp2;
		}
		ret = dw_configure_dai_by_pd(dev, dw_i2s_dai, res, pdata);
	} else {
		clk_id = "i2sclk";
		ret = dw_configure_dai_by_dt(dev, dw_i2s_dai, res);

		dev->clk = devm_clk_get(&pdev->dev, clk_id);
	}
	if (ret < 0)
		return ret;

	if (dev->capability & DW_I2S_MASTER) {
		if (pdata) {
			dev->i2s_clk_cfg = pdata->i2s_clk_cfg;
			if (!dev->i2s_clk_cfg) {
				dev_err(&pdev->dev, "no clock configure method\n");
				return -ENODEV;
			}
		}
		dev->clk = devm_clk_get(&pdev->dev, clk_id);

		if (IS_ERR(dev->clk))
			return PTR_ERR(dev->clk);

		ret = clk_prepare_enable(dev->clk);
		if (ret < 0) {
			dev_err(&pdev->dev, "I2S clock prepare failed\n");
			return ret;
		}
	}

#if defined(CONFIG_ARCH_BM1880)
/* Set mclk_out_en to 0 and mclk_in as clock resouce */
	i2s_write_reg(dev->i2s_base, I2S_USER_REG_CTL, I2S_USER_MCLK_OUT_DISABLE | I2S_USER_MCLK_IN);
#endif

	dev_set_drvdata(&pdev->dev, dev);
	ret = devm_snd_soc_register_component(&pdev->dev, &dw_i2s_component,
					 dw_i2s_dai, 1);
	if (ret != 0) {
		dev_err(&pdev->dev, "not able to register dai\n");
		goto err_clk_disable;
	}

	if (!pdata) {
		ret = devm_snd_dmaengine_pcm_register(&pdev->dev, NULL, 0);

		if (ret == -EPROBE_DEFER) {
			dev_err(&pdev->dev,
				"failed to register PCM, deferring probe\n");
			return ret;
		} else if (ret) {
			dev_err(&pdev->dev,
				"Could not register DMA PCM: %d\n"
				"falling back to PIO mode\n", ret);
			ret = dw_pcm_register(pdev);
			if (ret) {
				dev_err(&pdev->dev,
					"Could not register PIO PCM: %d\n",
					ret);
				goto err_clk_disable;
			}
		}
	}

	pm_runtime_enable(&pdev->dev);

	/* Enable I2S interrupts*/
	port = of_alias_get_id(np, "i2s");
	bm_enable_i2s_int(port, true);
	bm_enable_sysdma_int(true);

	return 0;

err_clk_disable:
	if (dev->capability & DW_I2S_MASTER)
		clk_disable_unprepare(dev->clk);
	return ret;
}

static int dw_i2s_remove(struct platform_device *pdev)
{
	struct dw_i2s_dev *dev = dev_get_drvdata(&pdev->dev);

	if (dev->capability & DW_I2S_MASTER)
		clk_disable_unprepare(dev->clk);

	pm_runtime_disable(&pdev->dev);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id dw_i2s_of_match[] = {
	{ .compatible = "snps,designware-i2s",	 },
	{},
};

MODULE_DEVICE_TABLE(of, dw_i2s_of_match);
#endif

static const struct dev_pm_ops dwc_pm_ops = {
	SET_RUNTIME_PM_OPS(dw_i2s_runtime_suspend, dw_i2s_runtime_resume, NULL)
};

static struct platform_driver dw_i2s_driver = {
	.probe		= dw_i2s_probe,
	.remove		= dw_i2s_remove,
	.driver		= {
		.name	= "designware-i2s",
		.of_match_table = of_match_ptr(dw_i2s_of_match),
		.pm = &dwc_pm_ops,
	},
};

module_platform_driver(dw_i2s_driver);

MODULE_AUTHOR("EthanChen <ethan.chen@bitmain.com>");
MODULE_DESCRIPTION("BITMAIN I2S SoC Interface");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:bitmain_i2s");