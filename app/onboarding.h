#ifndef NETPLAY_ONBOARDING_H
#define NETPLAY_ONBOARDING_H

#include <stdbool.h>
#include "netsetup.h"

#define OB_MAX_PAGES 24
#define OB_MAX_OPTIONS 6

typedef struct { char label[48]; int value; } OB_Option;
typedef struct {
	char title[80], description[768], image[256], setting[48];
	OB_Option options[OB_MAX_OPTIONS];
	int option_count;
} OB_Page;

bool OB_load(const char* path);
int OB_count(void);
const OB_Page* OB_page(int index);
int OB_settingValue(const char* key, const NS_Settings* settings);
bool OB_apply(const char* key, int value, NS_Settings* settings);

#endif
