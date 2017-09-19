/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */

/*
This is example code for the esphttpd library. It's a small-ish demo showing off 
the server, including WiFi connection management capabilities, some IO and
some pictures of cats.
*/

#include "sdkconfig.h"
#include "httpd.h"
#include "httpdespfs.h"
#include "cgiwifi.h"
#include "cgiflash.h"
#include "cgiappfs.h"
#include "auth.h"
#include "espfs.h"
#include "captdns.h"
#include "webpages-espfs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_spi_flash.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "soc/timer_group_struct.h"
#include "soc/dport_reg.h"

#include "rom/rtc.h"
#include "soc/soc.h"
#include "soc/rtc.h"
#include "soc/rtc_cntl_reg.h"

#include "appfs.h"
#include "8bkc-hal.h"
#include "8bkc-ugui.h"
#include "8bkcgui-widgets.h"

#include "gui.h"
#include "ugui.h"
#include "esp_log.h"

esp_err_t event_handler(void *ctx, system_event_t *event)
{
    return ESP_OK;
}

/*
This is the main url->function dispatching data struct.
In short, it's a struct with various URLs plus their handlers. The handlers can
be 'standard' CGI functions you wrote, or 'special' CGIs requiring an argument.
They can also be auth-functions. An asterisk will match any url starting with
everything before the asterisks; "*" matches everything. The list will be
handled top-down, so make sure to put more specific rules above the more
general ones. Authorization things (like authBasic) act as a 'barrier' and
should be placed above the URLs they protect.
*/
HttpdBuiltInUrl builtInUrls[]={
	{"*", cgiRedirectApClientToHostname, "esp8266.nonet"},
	{"/", cgiRedirect, "/index.html"},
	{"/reboot.cgi", cgiRebootFirmware, NULL},
	{"/upload.cgi", cgiUploadFile, NULL},
	{"/fileidx.cgi", cgiFileIdx, NULL},
	{"/delete.cgi", cgiDelete, NULL},
	{"*", cgiEspFsHook, NULL}, //Catch-all cgi function for the filesystem
	{NULL, NULL, NULL}
};


//Hack: Call ssd driver directly
void ssd1331SetContrast(int ctr);

void handleCharging() {
	int r;
	int fullCtr=0;

	//Force contrast low to decrease chance of burn-in
	ssd1331SetContrast(32);
	printf("Detected charger.\n");
	guiInit();
	guiCharging();

	//Disable app cpu
	DPORT_SET_PERI_REG_MASK(DPORT_APPCPU_CTRL_B_REG, DPORT_APPCPU_CLKGATE_EN);
	//Speed down
	rtc_clk_cpu_freq_set(RTC_CPU_FREQ_2M);

	do {
		r=kchal_get_chg_status();
		if (r==KC_CHG_CHARGING) {
			guiCharging();
			printf("Charging...\n");
			fullCtr=0;
		} else if (r==KC_CHG_FULL) {
			guiFull();
			printf("Full!\n");
			fullCtr++;
		}
		if (kchal_get_keys() & KC_BTN_POWER) {
			rtc_clk_cpu_freq_set(RTC_CPU_FREQ_80M);
			printf("Power btn pressed; starting\n");
			return;
		}
		if (fullCtr==32) {
			kchal_cal_adc();
		}
		vTaskDelay(1);
	} while (r!=KC_CHG_NOCHARGER);
	printf("Charger gone. Shutting down.\n");

	rtc_clk_cpu_freq_set(RTC_CPU_FREQ_80M);
	kchal_power_down();
}



void do_recovery_mode() {
	//Start + select are pressed while turning on or resetting. We haven't touched appfs or nvram
	//yet; give user option to nuke either or both.
	
	kcugui_menuitem_t menu[]={
		{"   Exit    ",0,NULL},
		{"Erase Flash",0,NULL},
		{" Reset NVS ",0,NULL},
		{"Factory Rst",0,NULL},
		{"",KCUGUI_MENUITEM_LAST,0,NULL}
	};
	
	guiInit();
	int i=kcugui_menu(menu, "RECOVERY", NULL, NULL);
	printf("Recovery menu choice: %d\n", i);
	if (i==1 || i==3) {
		esp_err_t r=ESP_OK;
		printf("Kill appfs\n");
		const esp_partition_t *p=esp_partition_find_first(APPFS_PART_TYPE, APPFS_PART_SUBTYPE, NULL);
		for (int i=0; i<p->size; i+=0x10000) {
			kcugui_cls();
			UG_FontSelect(&FONT_6X8);
			UG_SetForecolor(C_YELLOW);
			UG_PutString(0, 0, "ERASING...");
			UG_SetForecolor(C_WHITE);
			char pct[10];
			sprintf(pct, "%d%%", (i*100)/p->size);
			UG_PutString(0, 8*3, pct);
			kcugui_flush();
			esp_err_t lr=esp_partition_erase_range(p, i, 0x10000);
			if (lr!=ESP_OK) r=lr;
			vTaskDelay(1);
		}
		if (r!=ESP_OK) {
			printf("Couldn't erase: %d\n", r);
			UG_PutString(0, 8, "FAILED");
			vTaskDelay(3000/portTICK_RATE_MS);
		}
	} else if (i==2 || i==3) {
		printf("Kill nvs\n");
		const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
		kcugui_cls();
		UG_FontSelect(&FONT_6X8);
		UG_SetForecolor(C_YELLOW);
		UG_PutString(0, 0, "CLEARING...");
		kcugui_flush();
		esp_err_t r=esp_partition_erase_range(p, 0, p->size);
		if (r!=ESP_OK) {
			printf("Couldn't erase: %d\n", r);
			UG_PutString(0, 8, "FAILED");
			vTaskDelay(3000/portTICK_RATE_MS);
		}
	}
	kcugui_deinit();
}

//Main routine. Initialize stdout, the I/O, filesystem and the webserver and we're done.
int app_main(void)
{
	kchal_init_hw();
	if (kchal_get_keys() == (KC_BTN_START|KC_BTN_SELECT)) do_recovery_mode();
	kchal_init_sdk();
	if (kchal_get_chg_status()!=KC_CHG_NOCHARGER) handleCharging();

//	esp_log_level_set("*", ESP_LOG_INFO);
//	esp_log_level_set("appfs", ESP_LOG_DEBUG);

	appfsDump();
	if (appfsExists(UPLOAD_TEMP_NAME)) {
		printf("Deleting aborted upload file.\n");
		appfsDeleteFile(UPLOAD_TEMP_NAME);
	}

	printf("Starting webserver...\n");
	nvs_flash_init();
	tcpip_adapter_init();
	ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_AP) );
	wifi_config_t ap_config = {
		.ap = {
			.ssid = "gbfemto",
			.authmode=WIFI_AUTH_OPEN,
			.max_connection = 2,
		}
	};
	ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &ap_config) );
	ESP_ERROR_CHECK( esp_wifi_start() );
//	ESP_ERROR_CHECK( esp_wifi_connect() );

//	captdnsInit();

	espFsInit((void*)(webpages_espfs_start));
	httpdInit(builtInUrls, 80);

	guiInit();

	printf("\nReady\n");

	guiMenu();

	return 0;
}

