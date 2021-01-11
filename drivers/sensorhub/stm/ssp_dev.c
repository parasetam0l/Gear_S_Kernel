/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
#include "ssp.h"
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif
#if SSP_SEC_DEBUG
#include <mach/sec_debug.h>
#endif


#ifdef CONFIG_HAS_EARLYSUSPEND
static void ssp_early_suspend(struct early_suspend *handler);
static void ssp_late_resume(struct early_suspend *handler);
#endif

static int ssp_check_lpmode(void)
{
#ifdef CONFIG_SAMSUNG_LPM_MODE
	if (boot_mode_lpm == 0x01)
		return true;
	else
		return false;
#else
	return false;
#endif
}

void ssp_enable(struct ssp_data *data, bool enable)
{
	pr_info("[SSP] %s, enable = %d, old enable = %d\n",
		__func__, enable, data->bSspShutdown);

	if (enable && data->bSspShutdown) {
		data->bSspShutdown = false;
		enable_irq(data->iIrq);
		enable_irq_wake(data->iIrq);
	} else if (!enable && !data->bSspShutdown) {
		data->bSspShutdown = true;
		disable_irq(data->iIrq);
		disable_irq_wake(data->iIrq);
	} else {
		pr_err("[SSP] %s, error / enable = %d, old enable = %d\n",
			__func__, enable, data->bSspShutdown);
	}
}
/************************************************************************/
/* interrupt happened due to transition/change of SSP MCU		*/
/************************************************************************/

static irqreturn_t sensordata_irq_thread_fn(int iIrq, void *dev_id)
{
	struct ssp_data *data = dev_id;

	select_irq_msg(data);
	data->uIrqCnt++;

	return IRQ_HANDLED;
}

/*************************************************************************/
/* initialize sensor hub						 */
/*************************************************************************/

static void initialize_variable(struct ssp_data *data)
{
	int iSensorIndex;

	for (iSensorIndex = 0; iSensorIndex < SENSOR_MAX; iSensorIndex++) {
		data->adDelayBuf[iSensorIndex] = DEFUALT_POLLING_DELAY;
		data->batchLatencyBuf[iSensorIndex] = 0;
		data->batchOptBuf[iSensorIndex] = 0;
		data->aiCheckStatus[iSensorIndex] = INITIALIZATION_STATE;
	}
#ifdef CONFIG_SENSORS_SSP_ADPD142
	data->adDelayBuf[BIO_HRM_LIB] = (100 * NSEC_PER_MSEC);
#endif

	atomic_set(&data->aSensorEnable, 0);
	data->iLibraryLength = 0;
	data->uSensorState = 0;

	data->uResetCnt = 0;
	data->uTimeOutCnt = 0;
	data->uComFailCnt = 0;
	data->uIrqCnt = 0;

	data->bSspShutdown = true;
	data->bGeomagneticRawEnabled = false;
	data->bAccelAlert = false;
	data->bLpModeEnabled = false;
	data->bTimeSyncing = true;

	data->accelcal.x = 0;
	data->accelcal.y = 0;
	data->accelcal.z = 0;

	data->gyrocal.x = 0;
	data->gyrocal.y = 0;
	data->gyrocal.z = 0;

	data->magoffset.x = 0;
	data->magoffset.y = 0;
	data->magoffset.z = 0;

#ifdef CONFIG_SENSORS_SSP_LPS25H
	data->iPressureCal = 0;
	data->sealevelpressure = 0;
#endif
#ifdef CONFIG_SENSORS_SSP_ADPD142
	data->hrmcal.osc_reg = 0;
	data->hrmcal.green_70ma = 0;
	data->hrmcal.green_250ma = 0;
	data->hrmcal.red_70ma = 0;
	data->hrmcal.ir_70ma = 0;
#endif

	data->uGyroDps = GYROSCOPE_DPS2000;
	data->uIr_Current = DEFUALT_IR_CURRENT;

	data->mcu_device = NULL;
	data->acc_device = NULL;
	data->gyro_device = NULL;
	data->mag_device = NULL;
#ifdef CONFIG_SENSORS_SSP_LPS25H
	data->prs_device = NULL;
#endif
#ifdef CONFIG_SENSORS_SSP_AL3320
	data->light_device = NULL;
#endif
#ifdef CONFIG_SENSORS_SSP_ADPD142
	data->hrm_device = NULL;
#endif
#ifdef CONFIG_SENSORS_SSP_UVIS25
	data->uv_device = NULL;
#endif
	data->voice_device = NULL;
#if SSP_SEC_DEBUG
	data->bMcuDumpMode = sec_debug_is_enabled();
#endif
	INIT_LIST_HEAD(&data->pending_list);

	initialize_function_pointer(data);
}

int initialize_mcu(struct ssp_data *data)
{
	int iRet = 0;

	clean_pending_list(data);

	iRet = get_chipid(data);
	pr_info("[SSP] MCU device ID = %d, reading ID = %d\n", DEVICE_ID, iRet);
	if (iRet != DEVICE_ID) {
		if (iRet < 0) {
			pr_err("[SSP]: %s - MCU is not working : 0x%x\n",
				__func__, iRet);
		} else {
			pr_err("[SSP]: %s - MCU identification failed\n",
				__func__);
			iRet = -ENODEV;
		}
		goto out;
	}

	iRet = set_sensor_position(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - set_sensor_position failed\n", __func__);
		goto out;
	}

	iRet = set_magnetic_static_matrix(data);
	if (iRet < 0)
		pr_err("[SSP] %s - set_magnetic_static_matrix failed\n",
			__func__);

	data->uSensorState = get_sensor_scanning_info(data);
	if (data->uSensorState == 0) {
		pr_err("[SSP]: %s - get_sensor_scanning_info failed\n",
			__func__);
		iRet = ERROR;
		goto out;
	}

	iRet = get_fuserom_data(data);
	if (iRet < 0)
		pr_err("[SSP]: %s - get_fuserom_data failed\n", __func__);

	data->uCurFirmRev = get_firmware_rev(data);
	pr_info("[SSP] MCU Firm Rev : New = %8u\n",
		data->uCurFirmRev);

	iRet = ssp_send_cmd(data, MSG2SSP_AP_MCU_DUMP_CHECK, 0);
out:
	return iRet;
}

static int initialize_irq(struct ssp_data *data)
{
	int iRet;

	iRet = gpio_request(data->mcu_int1, "mcu_ap_int");
	if (iRet < 0) {
		pr_err("[SSP] %s, gpio %d request failed (%d)\n",
		       __func__, data->mcu_int1, iRet);
		return iRet;
	}

	iRet = gpio_direction_input(data->mcu_int1);
	if (iRet < 0) {
		pr_err("[SSP] %s, failed to set gpio %d as input (%d)\n",
		       __func__, data->mcu_int1, iRet);
		goto err_irq_direction_input;
	}

	pr_info("[SSP] requesting IRQ %d\n", data->iIrq);
	iRet = request_threaded_irq(data->iIrq, NULL, sensordata_irq_thread_fn,
				    IRQF_TRIGGER_FALLING|IRQF_ONESHOT,
				    "SSP_Int", data);
	if (iRet < 0) {
		pr_err("[SSP] %s, request_irq(%d) failed for gpio %d (%d)\n",
		       __func__, data->iIrq, data->iIrq, iRet);
		goto err_request_irq;
	}

	/* start with interrupts disabled */
	disable_irq(data->iIrq);
	return 0;

err_request_irq:
err_irq_direction_input:
	gpio_free(data->mcu_int1);
	return iRet;
}

static void work_function_firmware_update(struct work_struct *work)
{
	struct ssp_data *data = container_of((struct delayed_work *)work,
				struct ssp_data, work_firmware);
	int iRet;

	pr_info("[SSP] %s, is called\n", __func__);

	iRet = forced_to_download_binary(data, KERNEL_BINARY);
	if (iRet < 0) {
		ssp_dbg("[SSP] %s, forced_to_download_binary failed!\n",
			__func__);
		return;
	}

	queue_refresh_task(data, SSP_SW_RESET_TIME);

	if (data->check_lpmode() == true) {
		data->bLpModeEnabled = true;
		pr_info("[SSP]: LPM Charging...\n");
	} else {
		data->bLpModeEnabled = false;
		pr_info("[SSP]: Normal Booting OK\n");
	}

	pr_info("[SSP] : %s done\n!", __func__);
}

static int sensor_regulator_control(struct ssp_data *data, bool enable)
{
	struct regulator *acc_sensor;
	int errorno = 0;

	/* 9-axis sensor regulator control */
	acc_sensor = regulator_get(NULL, data->acc_vreg);
	if (IS_ERR(acc_sensor)) {
		acc_sensor = NULL;
		pr_err("[SSP] regulator_get Error\n");
		errorno = -EINVAL;
		goto exit;
	}

	if (enable) {
		errorno = regulator_set_voltage(acc_sensor, REGULATOR_VOL, REGULATOR_VOL);
		if (errorno) {
			pr_err("[SSP] regulator_get Error\n");
			goto exit;
		} else {
			errorno = regulator_enable(acc_sensor);
			if (errorno) {
				pr_err("[SSP] VDD can't turn on for Accel\n");
				goto exit;
			}
		}
	}
	else {
		errorno = regulator_disable(acc_sensor);
		if (errorno)
			pr_err("[SSP] VDD can't turn off for Accel\n");
	}

exit:
	return errorno;
}

#ifdef CONFIG_OF
static int ssp_parse_dt(struct device *dev,
	struct  ssp_data *data)
{
	struct device_node *np = dev->of_node;
	int errorno = 0;

	data->mcu_int1 = of_get_named_gpio(np, "ssp,mcu_int1-gpio", 0);
	if (data->mcu_int1 < 0) {
		pr_err("[SSP] %s, could not get mcu_int1-gpio\n", __func__);
		errorno = data->mcu_int1;
		goto dt_exit;
	}

	data->mcu_int2 = of_get_named_gpio(np, "ssp,mcu_int2-gpio", 0);
	if (data->mcu_int2 < 0) {
		pr_err("[SSP] %s, could not get mcu_int2-gpio\n", __func__);
		errorno = data->mcu_int2;
		goto dt_exit;
	}

	data->ap_int = of_get_named_gpio(np, "ssp,ap_int-gpio", 0);
	if (data->ap_int < 0) {
		pr_err("[SSP] %s, could not get ap_int-gpio\n", __func__);
		errorno = data->ap_int;
		goto dt_exit;
	}

	data->rst = of_get_named_gpio(np, "ssp,rst-gpio", 0);
	if (data->rst < 0) {
		pr_err("[SSP] %s, could not get rst-gpio\n", __func__);
		errorno = data->rst ;
		goto dt_exit;
	}

	if (of_property_read_u32(np, "ssp,acc-position", &data->accel_position))
		data->accel_position = 0;

	if (of_property_read_u32(np, "ssp,mag-position", &data->mag_position))
		data->accel_position = 0;

	if (of_property_read_u32(np, "ssp,ap-rev", &data->ap_rev))
		data->ap_rev = 0;

	if (of_property_read_string(np, "ssp,fw_name", &data->fw_name))
		pr_err("[SSP] %s, fail to get fw name\n", __func__);

	if (of_property_read_string(np, "ssp,ums_fw_name", &data->ums_fw_name))
		pr_err("[SSP] %s, fail to get ums_fw name\n", __func__);

	if (of_property_read_string(np, "ssp,acc_vreg", &data->acc_vreg))
		pr_err("[SSP] %s, fail to get vreg_acc name\n", __func__);

	data->iIrq = gpio_to_irq(data->mcu_int1);

	errorno = gpio_request(data->mcu_int2, "MCU_INT2");
	if (errorno) {
		pr_err("[SSP] %s, failed to request MCU_INT2 for SSP\n",
			__func__);
		goto dt_exit;
	}
	gpio_direction_input(data->mcu_int2);

	errorno = gpio_request(data->ap_int, "AP_MCU_INT");
	if (errorno) {
		pr_err("[SSP] %s, failed to request AP_INT for SSP\n",
			__func__);
		goto dt_exit;
	}
	gpio_direction_output(data->ap_int, 1);

	errorno = gpio_request(data->rst, "MCU_RST");
	if (errorno) {
		printk(KERN_ERR "failed to request MCU_RST for SSP\n");
		goto dt_exit;
	}
	gpio_direction_output(data->rst, 1);

dt_exit:
	return errorno;
}
#else
static int ssp_parse_dt(struct device *dev,
	struct  ssp_data *data)
{
	return -ENODEV;
}
#endif

static int ssp_probe(struct spi_device *spi)
{
	struct ssp_data *data;
	struct ssp_platform_data *pdata = NULL;
	int iRet = 0;

	pr_info("[SSP] %s, is called\n", __func__);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		pr_err("[SSP] %s, failed to allocate memory for data\n",
			__func__);
		iRet = -ENOMEM;
		goto exit;
	}

	if (spi->dev.of_node) {
		iRet = ssp_parse_dt(&spi->dev, data);
		if (iRet) {
			pr_err("[SSP]: %s - Failed to parse DT(error=%d)\n", __func__, iRet);
			goto exit;
		}
	} else {
		pdata = spi->dev.platform_data;
		if (pdata == NULL) {
			pr_err("[SSP] %s, platform_data is null\n", __func__);
			iRet = -ENOMEM;
			goto exit;
		}
	
		data->set_mcu_reset = pdata->set_mcu_reset;
		data->rst = pdata->rst;
		data->ap_int = pdata->ap_int;
		data->mcu_int1 = pdata->mcu_int1;
		data->mcu_int2 = pdata->mcu_int2;
		data->iIrq = pdata->irq;
		data->check_lpmode = pdata->check_lpmode;
#ifdef CONFIG_SENSORS_SSP_ADPD142
		data->hrm_sensor_power = pdata->hrm_sensor_power;
#endif
	/* AP system_rev */
		if (pdata->check_ap_rev)
			data->ap_rev = pdata->check_ap_rev();
		else
			data->ap_rev = 0;

		/* Get sensor positions */
		if (pdata->get_positions) {
			pdata->get_positions(&data->accel_position,
				&data->mag_position);
		} else {
			data->accel_position = 0;
			data->mag_position = 0;
		}
		if (pdata->mag_matrix) {
			data->mag_matrix_size = pdata->mag_matrix_size;
			data->mag_matrix = pdata->mag_matrix;
		}

		if (data->set_mcu_reset == NULL) {
			pr_err("[SSP] %s, function callback is null\n", __func__);
			iRet = -EIO;
			goto err_reset_null;
		}
	}
	sensor_regulator_control(data, 1);
	pr_info("[SSP] %s, rst = %d, ap_int = %d, mcu_int1 = %d, mcu_int2 = %d\n",
		__func__, (int)data->rst, (int)data->ap_int,
		(int)data->mcu_int1, (int)data->mcu_int2);
	pr_info("[SSP] %s, system Rev = 0x%x\n", __func__, data->ap_rev);

	data->check_lpmode = ssp_check_lpmode;

	spi->mode = SPI_MODE_1;
	if (spi_setup(spi)) {
		pr_err("[SSP] %s, failed to setup spi\n", __func__);
		goto err_setup;
	}

	data->bProbeIsDone = false;
	data->fw_dl_state = FW_DL_STATE_NONE;
	data->spi = spi;
	spi_set_drvdata(spi, data);

#ifdef CONFIG_SENSORS_SSP_STM
	mutex_init(&data->comm_mutex);
	mutex_init(&data->pending_mutex);
#endif

	pr_info("\n#####################################################\n");

	initialize_variable(data);
	INIT_DELAYED_WORK(&data->work_firmware, work_function_firmware_update);
	INIT_DELAYED_WORK(&data->work_refresh, refresh_task);

	wake_lock_init(&data->ssp_wake_lock,
		WAKE_LOCK_SUSPEND, "ssp_wake_lock");

	iRet = initialize_input_dev(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create input device\n", __func__);
		goto err_input_register_device;
	}

	iRet = initialize_debug_timer(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create workqueue\n", __func__);
		goto err_create_workqueue;
	}

	iRet = intialize_lpm_motion(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create workqueue\n", __func__);
		goto err_create_lpm_motion;
	}

	iRet = initialize_irq(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create irq\n", __func__);
		goto err_setup_irq;
	}

	iRet = initialize_sysfs(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create sysfs\n", __func__);
		goto err_sysfs_create;
	}

	iRet = initialize_event_symlink(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create symlink\n", __func__);
		goto err_symlink_create;
	}

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	/* init sensorhub device */
	iRet = ssp_sensorhub_initialize(data);
	if (iRet < 0) {
		pr_err("%s: ssp_sensorhub_initialize err(%d)", __func__, iRet);
		ssp_sensorhub_remove(data);
	}
#endif

	ssp_enable(data, true);
	/* check boot loader binary */
	data->fw_dl_state = check_fwbl(data);

	if (data->fw_dl_state == FW_DL_STATE_NONE) {
		iRet = initialize_mcu(data);
		if (iRet == ERROR) {
			toggle_mcu_reset(data);
		} else if (iRet < ERROR) {
			pr_err("[SSP] %s, initialize_mcu failed\n", __func__);
			goto err_read_reg;
		}
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.suspend = ssp_early_suspend;
	data->early_suspend.resume = ssp_late_resume;
	register_early_suspend(&data->early_suspend);
#endif

	pr_info("[SSP]: %s - probe success!\n", __func__);

	enable_debug_timer(data);

	if (data->fw_dl_state == FW_DL_STATE_NEED_TO_SCHEDULE) {
		pr_info("[SSP] Firmware update is scheduled\n");
		schedule_delayed_work(&data->work_firmware,
				msecs_to_jiffies(1000));
		data->fw_dl_state = FW_DL_STATE_SCHEDULED;
	} else if (data->fw_dl_state == FW_DL_STATE_FAIL) {
		data->bSspShutdown = true;
	}

	data->bProbeIsDone = true;
	iRet = 0;

	if (data->check_lpmode() == true) {
		ssp_charging_motion(data, 1);
		data->bLpModeEnabled = true;
		pr_info("[SSP]: LPM Charging...\n");
	} else {
		data->bLpModeEnabled = false;
		pr_info("[SSP]: Normal Booting OK\n");
	}

	goto exit;

err_read_reg:
err_symlink_create:
	remove_sysfs(data);
err_sysfs_create:
	free_irq(data->iIrq, data);
	gpio_free(data->mcu_int1);
err_setup_irq:
	destroy_workqueue(data->lpm_motion_wq);
err_create_lpm_motion:
	destroy_workqueue(data->debug_wq);
err_create_workqueue:
	remove_input_dev(data);
err_input_register_device:
	wake_lock_destroy(&data->ssp_wake_lock);
	cancel_delayed_work_sync(&data->work_firmware);
	cancel_delayed_work_sync(&data->work_refresh);
err_reset_null:
#ifdef CONFIG_SENSORS_SSP_STM
	mutex_destroy(&data->comm_mutex);
	mutex_destroy(&data->pending_mutex);
#endif

err_setup:
	kfree(data);
	pr_err("[SSP] %s, probe failed!\n", __func__);
exit:
	pr_info("#####################################################\n\n");
	return iRet;
}

static void ssp_shutdown(struct spi_device *spi)
{
	struct ssp_data *data = spi_get_drvdata(spi);

	func_dbg();
	atomic_set(&data->apShutdownProgress, 1);
	if (data->bProbeIsDone == false)
		goto exit;

	if (data->fw_dl_state >= FW_DL_STATE_SCHEDULED &&
		data->fw_dl_state < FW_DL_STATE_DONE) {
		pr_err("[SSP] %s, cancel_delayed_work_sync state = %d\n",
			__func__, data->fw_dl_state);
		cancel_delayed_work_sync(&data->work_firmware);
	}
	cancel_delayed_work(&data->work_refresh);

	if (SUCCESS != ssp_send_cmd(data, MSG2SSP_AP_STATUS_SHUTDOWN, 0))
		pr_err("[SSP]: %s MSG2SSP_AP_STATUS_SHUTDOWN failed\n",
			__func__);

	ssp_enable(data, false);
	disable_debug_timer(data);

	clean_pending_list(data);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->early_suspend);
#endif

	free_irq(data->iIrq, data);
	gpio_free(data->mcu_int1);

	remove_event_symlink(data);
	remove_sysfs(data);
	remove_input_dev(data);

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	ssp_sensorhub_remove(data);
#endif

	del_timer_sync(&data->debug_timer);
	cancel_work_sync(&data->work_debug);
	cancel_work_sync(&data->work_lpm_motion);
	destroy_workqueue(data->lpm_motion_wq);
	destroy_workqueue(data->debug_wq);
	wake_lock_destroy(&data->ssp_wake_lock);

#ifdef CONFIG_SENSORS_SSP_STM
	mutex_destroy(&data->comm_mutex);
	mutex_destroy(&data->pending_mutex);
#endif
	toggle_mcu_reset(data);

	sensor_regulator_control(data, 0);
	pr_info("[SSP] %s done\n", __func__);
exit:
	kfree(data);
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void ssp_early_suspend(struct early_suspend *handler)
{
	struct ssp_data *data;
	data = container_of(handler, struct ssp_data, early_suspend);

	func_dbg();
	disable_debug_timer(data);

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	/* give notice to user that AP goes to sleep */
	ssp_sensorhub_report_notice(data, MSG2SSP_AP_STATUS_SLEEP);
	ssp_sleep_mode(data);
	data->uLastAPState = MSG2SSP_AP_STATUS_SLEEP;
#else
	if (atomic_read(&data->aSensorEnable) > 0)
		ssp_sleep_mode(data);
#endif
}

static void ssp_late_resume(struct early_suspend *handler)
{
	struct ssp_data *data;
	data = container_of(handler, struct ssp_data, early_suspend);

	func_dbg();
	enable_debug_timer(data);

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	/* give notice to user that AP goes to sleep */
	ssp_sensorhub_report_notice(data, MSG2SSP_AP_STATUS_WAKEUP);
	ssp_resume_mode(data);
	data->uLastAPState = MSG2SSP_AP_STATUS_WAKEUP;
#else
	if (atomic_read(&data->aSensorEnable) > 0)
		ssp_resume_mode(data);
#endif
}

#else /* no early suspend */

static int ssp_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct ssp_data *data = spi_get_drvdata(spi);

	func_dbg();
	data->uLastResumeState = MSG2SSP_AP_STATUS_SUSPEND;
	disable_debug_timer(data);

	if (SUCCESS != ssp_send_cmd(data, MSG2SSP_AP_STATUS_SUSPEND, 0))
		pr_err("[SSP]: %s MSG2SSP_AP_STATUS_SUSPEND failed\n",
			__func__);
	data->bTimeSyncing = false;
	disable_irq(data->iIrq);
	return 0;
}

static int ssp_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct ssp_data *data = spi_get_drvdata(spi);
	enable_irq(data->iIrq);
	func_dbg();
	enable_debug_timer(data);

	if (SUCCESS != ssp_send_cmd(data, MSG2SSP_AP_STATUS_RESUME, 0))
		pr_err("[SSP]: %s MSG2SSP_AP_STATUS_RESUME failed\n",
			__func__);
	data->uLastResumeState = MSG2SSP_AP_STATUS_RESUME;

	return 0;
}

static const struct dev_pm_ops ssp_pm_ops = {
	.suspend = ssp_suspend,
	.resume = ssp_resume
};
#endif /* CONFIG_HAS_EARLYSUSPEND */

static const struct spi_device_id ssp_id[] = {
	{"ssp", 0},
	{}
};

MODULE_DEVICE_TABLE(spi, ssp_id);

#ifdef CONFIG_OF
static struct of_device_id ssp_match_table[] = {
	{ .compatible = "ssp,STM32F",},
	{},
};
#else
define ssp_match_table NULL
#endif

static struct spi_driver ssp_driver = {
	.probe = ssp_probe,
	.shutdown = ssp_shutdown,
	.id_table = ssp_id,
	.driver = {
#ifndef CONFIG_HAS_EARLYSUSPEND
		.pm = &ssp_pm_ops,
#endif
		.owner = THIS_MODULE,
		.name = "ssp",
		.of_match_table = ssp_match_table
	},
};

module_spi_driver(ssp_driver);
MODULE_DESCRIPTION("ssp spi driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
