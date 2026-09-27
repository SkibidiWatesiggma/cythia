#include <stdio.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char **argv){
	char *filepath = argv[1];

	FILE *map = fopen(filepath, "rb");

	if (NULL == map){
		perror("fopen");
		return 1;
	}

	char magic[4];
	fread(magic, 1, 4, map);

	if (memcmp(magic, "SS+m", 4) != 0){
		printf("not SSPM");
		fclose(map);
		return 2;
	}

	uint32_t version;
	fread(&version, 2, 1, map);

	if (version != 2){
		printf("not SSPM v2");
	 fclose(map);
		return 3;
	}

	return 0;
}
