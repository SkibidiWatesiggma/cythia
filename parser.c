#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <io.h>
#include <fcntl.h>

#define MAX_VALUES 32
#define MAX_STRING 65535

typedef struct {
	uint8_t type;
} ValueDef;

typedef struct {
	char id[256];
	uint8_t value_count;
	ValueDef values[MAX_VALUES];
} MarkerDef;

static int read_u8(FILE *f, uint8_t *v)
{
	return fread(v, 1, 1, f) == 1;
}

static int read_u16(FILE *f, uint16_t *v)
{
	return fread(v, 2, 1, f) == 1;
}

static int read_u32(FILE *f, uint32_t *v)
{
	return fread(v, 4, 1, f) == 1;
}

static int read_u64(FILE *f, uint64_t *v)
{
	return fread(v, 8, 1, f) == 1;
}

static int read_f32(FILE *f, float *v)
{
	return fread(v, 4, 1, f) == 1;
}

static int read_string(FILE *f, char **out)
{
	uint16_t length;
	char *str;

	if (!read_u16(f, &length))
		return 0;

	str = malloc((size_t)length + 1);

	if (str == NULL)
		return 0;

	if (fread(str, 1, length, f) != length){
		free(str);
		return 0;
	}

	str[length] = '\0';
	*out = str;

	return 1;
}

static int skip_value(FILE *f, uint8_t type)
{
	uint8_t mode;
	uint16_t length16;
	uint32_t length32;

	switch (type){
		case 0x01:
			return _fseeki64(f, 1, SEEK_CUR) == 0;

		case 0x02:
			return _fseeki64(f, 2, SEEK_CUR) == 0;

		case 0x03:
			return _fseeki64(f, 4, SEEK_CUR) == 0;

		case 0x04:
			return _fseeki64(f, 8, SEEK_CUR) == 0;

		case 0x05:
			return _fseeki64(f, 4, SEEK_CUR) == 0;

		case 0x06:
			return _fseeki64(f, 8, SEEK_CUR) == 0;

		case 0x07:
			if (!read_u8(f, &mode))
				return 0;

			if (mode == 0x00)
				return _fseeki64(f, 2, SEEK_CUR) == 0;

			if (mode == 0x01)
				return _fseeki64(f, 8, SEEK_CUR) == 0;

			return 0;

		case 0x08:
		case 0x09:
			if (!read_u16(f, &length16))
				return 0;

			return _fseeki64(f, length16, SEEK_CUR) == 0;

		case 0x0a:
		case 0x0b:
			if (!read_u32(f, &length32))
				return 0;

			return _fseeki64(f, (int64_t)length32, SEEK_CUR) == 0;

		default:
			return 0;
	}
}

static int read_position(FILE *f, float *x, float *y)
{
	uint8_t mode;

	if (!read_u8(f, &mode))
		return 0;

	if (mode == 0x00){
		uint8_t ix;
		uint8_t iy;

		if (!read_u8(f, &ix) ||
		    !read_u8(f, &iy))
			return 0;

		*x = (float)ix;
		*y = (float)iy;

		return 1;
	}

	if (mode == 0x01){
		if (!read_f32(f, x) ||
		    !read_f32(f, y))
			return 0;

		return 1;
	}

	return 0;
}

static int write_u16(FILE *f, uint16_t v)
{
	return fwrite(&v, 2, 1, f) == 1;
}

static int write_u32(FILE *f, uint32_t v)
{
	return fwrite(&v, 4, 1, f) == 1;
}

static int write_u64(FILE *f, uint64_t v)
{
	return fwrite(&v, 8, 1, f) == 1;
}

static int write_string(FILE *f, const char *str)
{
	size_t length = strlen(str);

	if (length > UINT16_MAX)
		return 0;

	if (!write_u16(f, (uint16_t)length))
		return 0;

	return fwrite(str, 1, length, f) == length;
}

typedef struct {
	uint32_t time;
	float x;
	float y;
} Note;

int main(int argc, char **argv)
{
	_setmode(_fileno(stdout), _O_BINARY);

	if (argc != 2){
		fprintf(stderr, "usage: %s <map.sspm>\n", argv[0]);
		return 1;
	}

	FILE *map = fopen(argv[1], "rb");

	if (map == NULL){
		perror("fopen");
		return 1;
	}

	char magic[4];

	if (fread(magic, 1, 4, map) != 4 ||
	    memcmp(magic, "SS+m", 4) != 0){
		fprintf(stderr, "not SSPM\n");
		fclose(map);
		return 2;
	}

	uint16_t version;

	if (!read_u16(map, &version) || version != 2){
		fprintf(stderr, "not SSPM v2\n");
		fclose(map);
		return 3;
	}

	uint32_t reserved;

	if (!read_u32(map, &reserved) || reserved != 0){
		fprintf(stderr, "invalid header\n");
		fclose(map);
		return 4;
	}

	uint8_t sha1[20];

	if (fread(sha1, 1, 20, map) != 20){
		fprintf(stderr, "failed to read sha1\n");
		fclose(map);
		return 5;
	}

	uint32_t last_marker_time;
	uint32_t note_count;
	uint32_t marker_count;

	if (!read_u32(map, &last_marker_time) ||
	    !read_u32(map, &note_count) ||
	    !read_u32(map, &marker_count)){
		fprintf(stderr, "failed to read metadata\n");
		fclose(map);
		return 5;
	}

	uint8_t difficulty;
	uint16_t rating;
	uint8_t has_audio;
	uint8_t has_cover;
	uint8_t requires_mod;

	if (!read_u8(map, &difficulty) ||
	    !read_u16(map, &rating) ||
	    !read_u8(map, &has_audio) ||
	    !read_u8(map, &has_cover) ||
	    !read_u8(map, &requires_mod)){
		fprintf(stderr, "failed to read metadata\n");
		fclose(map);
		return 5;
	}

	uint64_t custom_offset;
	uint64_t custom_length;
	uint64_t audio_offset;
	uint64_t audio_length;
	uint64_t cover_offset;
	uint64_t cover_length;
	uint64_t definitions_offset;
	uint64_t definitions_length;
	uint64_t markers_offset;
	uint64_t markers_length;

	if (!read_u64(map, &custom_offset) ||
	    !read_u64(map, &custom_length) ||
	    !read_u64(map, &audio_offset) ||
	    !read_u64(map, &audio_length) ||
	    !read_u64(map, &cover_offset) ||
	    !read_u64(map, &cover_length) ||
	    !read_u64(map, &definitions_offset) ||
	    !read_u64(map, &definitions_length) ||
	    !read_u64(map, &markers_offset) ||
	    !read_u64(map, &markers_length)){
		fprintf(stderr, "failed to read pointers\n");
		fclose(map);
		return 5;
	}

	char *map_id;
	char *map_name;
	char *song_name;

	if (!read_string(map, &map_id) ||
	    !read_string(map, &map_name) ||
	    !read_string(map, &song_name)){
		fprintf(stderr, "failed to read strings\n");
		fclose(map);
		return 6;
	}

	uint16_t mapper_count;

	if (!read_u16(map, &mapper_count)){
		fprintf(stderr, "failed to read mapper count\n");
		free(map_id);
		free(map_name);
		free(song_name);
		fclose(map);
		return 6;
	}

	for (uint16_t i = 0; i < mapper_count; i++){
		char *mapper;

		if (!read_string(map, &mapper)){
			fprintf(stderr, "failed to read mapper\n");
			free(map_id);
			free(map_name);
			free(song_name);
			fclose(map);
			return 6;
		}

		free(mapper);
	}

	if (_fseeki64(map, (long)definitions_offset, SEEK_SET) != 0){
		fprintf(stderr, "failed to seek definitions\n");
		free(map_id);
		free(map_name);
		free(song_name);
		fclose(map);
		return 7;
	}

	uint8_t definition_count;

	if (!read_u8(map, &definition_count)){
		fprintf(stderr, "failed to read definition count\n");
		free(map_id);
		free(map_name);
		free(song_name);
		fclose(map);
		return 7;
	}

	MarkerDef *definitions =
		calloc(definition_count, sizeof(MarkerDef));

	if (definitions == NULL){
		fprintf(stderr, "out of memory\n");
		free(map_id);
		free(map_name);
		free(song_name);
		fclose(map);
		return 8;
	}

	for (uint8_t i = 0; i < definition_count; i++){
		uint16_t id_length;

		if (!read_u16(map, &id_length) ||
		    id_length >= sizeof(definitions[i].id)){
			fprintf(stderr, "invalid marker definition\n");
			free(definitions);
			free(map_id);
			free(map_name);
			free(song_name);
			fclose(map);
			return 7;
		}

		if (fread(definitions[i].id, 1, id_length, map) != id_length){
			fprintf(stderr, "failed to read marker definition\n");
			free(definitions);
			free(map_id);
			free(map_name);
			free(song_name);
			fclose(map);
			return 7;
		}

		definitions[i].id[id_length] = '\0';

		if (!read_u8(map, &definitions[i].value_count) ||
		    definitions[i].value_count > MAX_VALUES){
			fprintf(stderr, "invalid marker definition values\n");
			free(definitions);
			free(map_id);
			free(map_name);
			free(song_name);
			fclose(map);
			return 7;
		}

		for (uint8_t j = 0; j < definitions[i].value_count; j++){
			if (!read_u8(map, &definitions[i].values[j].type)){
				fprintf(stderr, "failed to read marker value\n");
				free(definitions);
				free(map_id);
				free(map_name);
				free(song_name);
				fclose(map);
				return 7;
			}
		}

		uint8_t terminator;

		if (!read_u8(map, &terminator) || terminator != 0x00){
			fprintf(stderr, "invalid marker definition terminator\n");
			free(definitions);
			free(map_id);
			free(map_name);
			free(song_name);
			fclose(map);
			return 7;
		}
	}

	int note_definition = -1;

	for (uint8_t i = 0; i < definition_count; i++){
		if (strcmp(definitions[i].id, "ssp_note") == 0){
			note_definition = i;
			break;
		}
	}

	if (note_definition < 0){
		fprintf(stderr, "no ssp_note definition\n");
		free(definitions);
		free(map_id);
		free(map_name);
		free(song_name);
		fclose(map);
		return 9;
	}

	Note *notes = malloc(sizeof(Note) * note_count);

	if (notes == NULL && note_count != 0){
		fprintf(stderr, "out of memory\n");
		free(definitions);
		free(map_id);
		free(map_name);
		free(song_name);
		fclose(map);
		return 10;
	}

	if (_fseeki64(map, (long)markers_offset, SEEK_SET) != 0){
		fprintf(stderr, "failed to seek markers\n");
		free(notes);
		free(definitions);
		free(map_id);
		free(map_name);
		free(song_name);
		fclose(map);
		return 11;
	}

	uint32_t actual_notes = 0;

	for (uint32_t i = 0; i < marker_count; i++){
		uint32_t time;
		uint8_t marker_type;

		if (!read_u32(map, &time) ||
		    !read_u8(map, &marker_type)){
			fprintf(stderr, "failed to read marker\n");
			free(notes);
			free(definitions);
			free(map_id);
			free(map_name);
			free(song_name);
			fclose(map);
			return 11;
		}

		if (marker_type >= definition_count){
			fprintf(stderr, "invalid marker type\n");
			free(notes);
			free(definitions);
			free(map_id);
			free(map_name);
			free(song_name);
			fclose(map);
			return 11;
		}

		MarkerDef *def = &definitions[marker_type];

		float x = 0.0f;
		float y = 0.0f;
		int has_position = 0;

		for (uint8_t j = 0; j < def->value_count; j++){
			uint8_t type = def->values[j].type;

			if (type == 0x07){
				if (!read_position(map, &x, &y)){
					fprintf(stderr, "failed to read note position\n");
					free(notes);
					free(definitions);
					free(map_id);
					free(map_name);
					free(song_name);
					fclose(map);
					return 11;
				}

				has_position = 1;
			}
			else{
				if (!skip_value(map, type)){
					fprintf(stderr, "failed to skip marker data\n");
					free(notes);
					free(definitions);
					free(map_id);
					free(map_name);
					free(song_name);
					fclose(map);
					return 11;
				}
			}
		}

		if (marker_type == note_definition && has_position){
			if (actual_notes >= note_count){
				fprintf(stderr, "too many notes\n");
				free(notes);
				free(definitions);
				free(map_id);
				free(map_name);
				free(song_name);
				fclose(map);
				return 12;
			}

			notes[actual_notes].time = time;
			notes[actual_notes].x = x;
			notes[actual_notes].y = y;
			actual_notes++;
		}
	}

	if (!has_audio || audio_offset == 0 || audio_length == 0)
		audio_length = 0;

	if (audio_length > 0){
		if (_fseeki64(map, (int64_t)audio_offset, SEEK_SET) != 0){
			fprintf(stderr, "failed to seek audio\n");
			free(notes);
			free(definitions);
			free(map_id);
			free(map_name);
			free(song_name);
			fclose(map);
			return 13;
		}
	}

	fwrite("RMAP", 1, 4, stdout);

	if (!write_string(stdout, map_name) ||
	    !write_string(stdout, song_name) ||
	    !write_u32(stdout, actual_notes)){
		fprintf(stderr, "failed to write parser output\n");
		free(notes);
		free(definitions);
		free(map_id);
		free(map_name);
		free(song_name);
		fclose(map);
		return 14;
	}

	if (actual_notes > 0){
		if (fwrite(notes, sizeof(Note), actual_notes, stdout) != actual_notes){
			fprintf(stderr, "failed to write notes\n");
			free(notes);
			free(definitions);
			free(map_id);
			free(map_name);
			free(song_name);
			fclose(map);
			return 14;
		}
	}

	if (!write_u64(stdout, audio_length)){
		fprintf(stderr, "failed to write audio length\n");
		free(notes);
		free(definitions);
		free(map_id);
		free(map_name);
		free(song_name);
		fclose(map);
		return 14;
	}

	if (audio_length > 0){
		uint8_t buffer[65536];
		uint64_t remaining = audio_length;

		while (remaining > 0){
			size_t wanted =
				remaining > sizeof(buffer)
				? sizeof(buffer)
				: (size_t)remaining;

			size_t got = fread(buffer, 1, wanted, map);

			if (got != wanted){
				fprintf(stderr, "failed to read audio\n");
				free(notes);
				free(definitions);
				free(map_id);
				free(map_name);
				free(song_name);
				fclose(map);
				return 13;
			}

			if (fwrite(buffer, 1, got, stdout) != got){
				fprintf(stderr, "failed to write audio\n");
				free(notes);
				free(definitions);
				free(map_id);
				free(map_name);
				free(song_name);
				fclose(map);
				return 14;
			}

			remaining -= got;
		}
	}

	free(notes);
	free(definitions);
	free(map_id);
	free(map_name);
	free(song_name);
	fclose(map);

	return 0;
}