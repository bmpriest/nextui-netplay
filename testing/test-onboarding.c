#include "../app/onboarding.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static NS_Settings settings;
NS_Settings* NS_settings(void) { return &settings; }

int main(int argc, char** argv) {
	assert(argc == 2);
	assert(OB_load(argv[1]));
	assert(OB_count() == 3);
	assert(!strcmp(OB_page(1)->setting, "compatibility_cores"));
	assert(OB_page(1)->options[0].value == 1);
	assert(OB_apply("compatibility_cores", 1, &settings));
	assert(OB_settingValue("compatibility_cores", &settings) == 1);
	assert(!OB_apply("compatibility_cores", 2, &settings));
	assert(!OB_apply("quick_menu", 1, &settings));

	char path[] = "/tmp/netplay-onboarding-test-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);
	FILE* f = fdopen(fd, "w");
	assert(f);
	fputs("{\"pages\":[{\"title\":\"Question\",\"setting\":\"quick_menu\",\"options\":[{\"label\":\"Yes\",\"value\":1},{\"label\":\"No\",\"value\":0}]}]}", f);
	fclose(f);
	assert(!OB_load(path));
	assert(OB_count() == 0);
	unlink(path);
	return 0;
}
