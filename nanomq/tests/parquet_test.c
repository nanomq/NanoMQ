// #include "core/nng_impl.h"
#include "nanomq_test.h"
#include "nng/nng.h"
#include "nng/supplemental/nanolib/cvector.h"
#include "nng/supplemental/nanolib/parquet.h"
#include "nng/supplemental/nanolib/md5.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #include <nuts.h>

#define DO_IT_IF_NOT_NULL(func, arg1, arg2) \
	if (arg1) {                         \
		func(arg1, arg2);           \
	}

#define FREE_IF_NOT_NULL(free, size) DO_IT_IF_NOT_NULL(nng_free, free, size)

#define DATASIZE 10
#define NUM_KEYS 5
#define STRING_LENGTH 64
static uint64_t keys_test[NUM_KEYS][DATASIZE] = { 
	{ 10, 21, 32, 43, 54, 65, 76,87, 98, 109 },
	{ 110, 121, 132, 143, 154, 165, 176, 187, 198, 1109 },
	{ 220, 222, 232, 243, 254, 265, 276, 287, 298, 2209 },
	{ 330, 333, 333, 343, 354, 365, 376, 387, 398, 3309 },
	{ 440, 444, 444, 444, 454, 465, 476, 487, 498, 4409 } 
};

static uint64_t find_keys_test[NUM_KEYS] = { 10, 110, 220, 330, 440 };
static char     topic[]                  = "canudp";
static char     prefix[]                 = "/tmp/parquet/ly_canudp";
static char    *schema[]                 = { "ts", "data" };

static char *filenames[NUM_KEYS] = { "10~109.parquet", "110~1109.parquet",
	"220~2209.parquet", "330~3309.parquet", "440~4409.parquet" };

static char *full_filenames[NUM_KEYS] = { 0 };

typedef struct {
	nng_aio *aio;
} work;

uint64_t *
keys_allocate(uint64_t keys[], uint32_t size)
{
	uint32_t  i          = 0;
	uint64_t *keys_alloc = malloc(size * sizeof(uint64_t));
	while (i < size) {
		keys_alloc[i] = keys[i];
		i++;
	}
	return keys_alloc;
}

parquet_data_packet ***
parquet_data_packet_array_generate(uint32_t col_len, uint32_t row_len, bool generate_null)
{
	uint32_t               c = 0;
	uint32_t               r = 0;
	parquet_data_packet ***packet_matrix =
	    (parquet_data_packet ***) malloc(
	        col_len * sizeof(parquet_data_packet **));
	while (c < col_len) {
		packet_matrix[c] = (parquet_data_packet **) malloc(
		    row_len * sizeof(parquet_data_packet *));
		while (r < row_len) {
			if (generate_null && (r*c % 3) == 1) {
				packet_matrix[c][r] = NULL;
			} else {
				packet_matrix[c][r] =
				    (parquet_data_packet *) malloc(
				        sizeof(parquet_data_packet));
				char data[STRING_LENGTH] = { 0 };
				snprintf(data, STRING_LENGTH,
				    "hello world_%u_%u", c, r);
				packet_matrix[c][r]->size = strlen(data);
				packet_matrix[c][r]->data = strdup(data);
			}
			r++;
		}
		r = 0;
		c++;
	}

	return packet_matrix;
}

uint8_t **
data_array_allocate(uint32_t **dsize, uint32_t size)
{
	uint32_t  i           = 0;
	uint32_t *dsize_alloc = malloc(size * sizeof(uint32_t));
	while (i < size) {
		dsize_alloc[i] = STRING_LENGTH;
		i++;
	}

	char **darray = malloc(size * sizeof(char *));

	if (darray == NULL) {
		printf("Memory allocation failed. Exiting...\n");
		return NULL;
	}

	for (uint32_t i = 0; i < size; i++) {
		darray[i] = malloc((STRING_LENGTH + 1) * sizeof(char));

		if (darray[i] == NULL) {
			printf("Memory allocation failed for element %d. "
			       "Exiting...\n",
			    i);

			// Free previously allocated memory before exiting
			for (uint32_t j = 0; j < i; j++) {
				free(darray[j]);
			}
			free(darray);

			return NULL;
		}

		sprintf(darray[i], "hello world%d", i);
	}
	*dsize = dsize_alloc;

	return (uint8_t **) darray;
}


void
works_free(work **works)
{
	for (size_t i = 0; i < cvector_size(works); i++) {
		nng_aio_free(works[i]->aio);
		nng_free(works[i], sizeof(work));
	}
	cvector_free(works);
}
void
check_name(char *result, char *expect_suffix)
{
	char md5_buffer[MD5_LEN + 1];
	int  ret = ComputeFileMD5(result, md5_buffer);
	if (ret != 0) {
		log_error("Failed to calculate md5sum");
		goto error;
	}
	char filename[128] = { 0 };
	snprintf(filename, 128, "%s_%s-%s", prefix, md5_buffer, expect_suffix);

	check(nng_strcasecmp(result, filename) == 0,
	    "Filename error: %s != %s", result, filename);
	return;
error:
	abort();
}

void
aio_test_cb(void *arg)
{
	work	        *w           = (work *) arg;
	nng_aio             *aio         = w->aio;
	static int           test_index  = 0;
	parquet_file_ranges *file_ranges = nng_aio_get_output(aio, 1);
	// char	       **data_array  = nng_aio_get_prov_data(aio);

	check(file_ranges->size == 1, "file_ranges size error");

	for (int i = 0; i < file_ranges->size; i++) {
		parquet_file_range *range = file_ranges->range[i];
		check_mem(range);
		check(range->start_idx == 0, "Start Index error");
		check(range->end_idx == 9, "End Index error");
		check_name(range->filename, filenames[test_index]);
		// Copy for below test.
		full_filenames[test_index] = nng_strdup(range->filename);

	}
	test_index++;
	return;
error:
	puts("parquet write batch async failed!");
	abort();
}

void
aio_test_write_tmp_cb(void *arg)
{
	work	        *w           = (work *) arg;
	nng_aio             *aio         = w->aio;
	static int           test_index  = 0;
	parquet_file_ranges *file_ranges = nng_aio_get_output(aio, 1);
	char	       **data_array  = nng_aio_get_prov_data(aio);
	uint32_t            *len         = (uint32_t *) nng_aio_get_msg(aio);

	for (uint32_t i = 0; i < *len; i++) {
		if (data_array[i])
			nng_strfree(data_array[i]);
	}
	free(len);

	check(file_ranges->size == 1, "file_ranges size error");

	for (int i = 0; i < file_ranges->size; i++) {
		parquet_file_range *range = file_ranges->range[i];
		check_mem(range);
		check(range->start_idx == 0, "Start Index error");
		check(range->end_idx == 9, "End Index error");
		log_test("Filename: %s", range->filename);
	}
	test_index++;
	return;
error:
	puts("parquet write batch tmp async failed!");
	abort();
}

work *
parquet_write_batch_async_test1(void)
{
	uint64_t              *ts = keys_allocate(keys_test[0], DATASIZE);
	parquet_data_packet ***matrix =
	    parquet_data_packet_array_generate(1, DATASIZE, false);

	char **schema_l = malloc(sizeof(char*)*2);
	schema_l[0] = strdup("ts");
	schema_l[1] = strdup("data");

	parquet_data *data = parquet_data_alloc(schema_l, matrix, ts, 1, DATASIZE);

	work *w  = ALLOC_STRUCT(w);
	int   rv = 0;
	if ((rv = nng_aio_alloc(&w->aio, aio_test_cb, w)) != 0) {
		printf("nng_aio_alloc failed\n");
	}

	parquet_object *elem =
	    parquet_object_alloc(data, WRITE_RAW, w->aio, NULL, topic);

	parquet_write_batch_async(elem);
	return w;
}

work *
parquet_write_batch_async_test2(void)
{
	uint64_t              *ts = keys_allocate(keys_test[1], DATASIZE);
	parquet_data_packet ***matrix =
	    parquet_data_packet_array_generate(DATASIZE, DATASIZE, false);

	char **schema_l = malloc(sizeof(char *) * DATASIZE);
	schema_l[0]     = strdup("ts");
	schema_l[1]     = strdup("data123123");
	schema_l[2]     = strdup("data+23434");
	schema_l[3]     = strdup("data_123444");
	schema_l[4]     = strdup("data+222222");
	schema_l[5]     = strdup("data+4444444444444");
	schema_l[6]     = strdup("data+11111234");
	schema_l[7]     = strdup("data+11111111");
	schema_l[8]     = strdup("data+22222222");
	schema_l[9]     = strdup("dataaaaaaaaaaaa");

	parquet_data *data =
	    parquet_data_alloc(schema_l, matrix, ts, DATASIZE - 1, DATASIZE);

	work *w  = ALLOC_STRUCT(w);
	int   rv = 0;
	if ((rv = nng_aio_alloc(&w->aio, aio_test_cb, w)) != 0) {
		printf("nng_aio_alloc failed\n");
	}

	parquet_object *elem =
	    parquet_object_alloc(data, WRITE_RAW, w->aio, NULL, topic);

	parquet_write_batch_async(elem);
	return w;
}

work *
parquet_write_batch_async_test3(void)
{
	uint64_t              *ts = keys_allocate(keys_test[2], DATASIZE);
	parquet_data_packet ***matrix =
	    parquet_data_packet_array_generate(DATASIZE, DATASIZE, true);

	char **schema_l = malloc(sizeof(char *) * DATASIZE);
	schema_l[0]     = strdup("ts");
	schema_l[1]     = strdup("data123123");
	schema_l[2]     = strdup("data+23434");
	schema_l[3]     = strdup("data_123444");
	schema_l[4]     = strdup("data+222222");
	schema_l[5]     = strdup("data+4444444444444");
	schema_l[6]     = strdup("data+11111234");
	schema_l[7]     = strdup("data+11111111");
	schema_l[8]     = strdup("data+22222222");
	schema_l[9]     = strdup("dataaaaaaaaaaaa");

	parquet_data *data =
	    parquet_data_alloc(schema_l, matrix, ts, DATASIZE - 1, DATASIZE);

	work *w  = ALLOC_STRUCT(w);
	int   rv = 0;
	if ((rv = nng_aio_alloc(&w->aio, aio_test_cb, w)) != 0) {
		printf("nng_aio_alloc failed\n");
	}

	parquet_object *elem =
	    parquet_object_alloc(data, WRITE_RAW, w->aio, NULL, topic);

	parquet_write_batch_async(elem);
	return w;
}

// work *
// parquet_write_batch_async_test4(void)
// {
// 	uint32_t *dsize;
// 	uint64_t *keys   = keys_allocate(keys_test[3], DATASIZE);
// 	uint8_t **darray = data_array_allocate(&dsize, DATASIZE);
// 
// 	work *w  = ALLOC_STRUCT(w);
// 	int   rv = 0;
// 	if ((rv = nng_aio_alloc(&w->aio, aio_test_cb, w)) != 0) {
// 		printf("nng_aio_alloc failed\n");
// 	}
// 
// 	parquet_object *elem = parquet_object_alloc(
// 	    keys, (uint8_t **) darray, dsize, DATASIZE, w->aio, darray);
// 	elem->topic = topic;
// 
// 	parquet_write_batch_async(elem);
// 
// 	return w;
// }
// 
// work *
// parquet_write_batch_async_test5(void)
// {
// 	uint32_t *dsize;
// 	uint64_t *keys   = keys_allocate(keys_test[4], DATASIZE);
// 	uint8_t **darray = data_array_allocate(&dsize, DATASIZE);
// 
// 	work *w  = ALLOC_STRUCT(w);
// 	int   rv = 0;
// 	if ((rv = nng_aio_alloc(&w->aio, aio_test_cb, w)) != 0) {
// 		printf("nng_aio_alloc failed\n");
// 	}
// 
// 	parquet_object *elem = parquet_object_alloc(
// 	    keys, (uint8_t **) darray, dsize, DATASIZE, w->aio, darray);
// 	elem->topic = topic;
// 
// 	parquet_write_batch_async(elem);
// 
// 	return w;
// }
// 
// work *
// parquet_write_batch_tmp_async_test1(void)
// {
// 	uint32_t *dsize;
// 	uint64_t *keys   = keys_allocate(keys_test[0], DATASIZE);
// 	uint8_t **darray = data_array_allocate(&dsize, DATASIZE);
// 
// 	work *w  = ALLOC_STRUCT(w);
// 	int   rv = 0;
// 	if ((rv = nng_aio_alloc(&w->aio, aio_test_write_tmp_cb, w)) != 0) {
// 		printf("nng_aio_alloc failed\n");
// 	}
// 
// 	parquet_object *elem = parquet_object_alloc(
// 	    keys, (uint8_t **) darray, dsize, DATASIZE, w->aio, darray);
// 
// 	parquet_write_batch_tmp_async(elem);
// 
// 	return w;
// }
// 
// work *
// parquet_write_batch_tmp_async_test2(void)
// {
// 	uint32_t *dsize;
// 	uint64_t *keys   = keys_allocate(keys_test[1], DATASIZE);
// 	uint8_t **darray = data_array_allocate(&dsize, DATASIZE);
// 
// 	work *w  = ALLOC_STRUCT(w);
// 	int   rv = 0;
// 	if ((rv = nng_aio_alloc(&w->aio, aio_test_write_tmp_cb, w)) != 0) {
// 		printf("nng_aio_alloc failed\n");
// 	}
// 
// 	parquet_object *elem = parquet_object_alloc(
// 	    keys, (uint8_t **) darray, dsize, DATASIZE, w->aio, darray);
// 
// 	parquet_write_batch_tmp_async(elem);
// 
// 	return w;
// }

void
clear_folder(const char *folderPath)
{
	DIR *dir = opendir(folderPath);
	if (dir == NULL) {
		fprintf(stderr, "Failed to open directory: %s\n", folderPath);
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		char filePath[512];
		snprintf(filePath, sizeof(filePath), "%s/%s", folderPath,
		    entry->d_name);

		if (remove(filePath) != 0) {
			fprintf(stderr,
			    "Failed to remove file/directory: %s\n", filePath);
		}
	}

	closedir(dir);
}

conf_parquet *
conf_parquet_init()
{

	conf_parquet *conf      = ALLOC_STRUCT(conf);
	conf->enable            = true;
	conf->dir               = strdup("/tmp/parquet");
	conf->file_name_prefix  = strdup("ly");
	conf->comp_type         = UNCOMPRESSED;
	conf->file_count        = 5;
	conf->file_index        = 0;
	conf->file_size         = 4000;
	conf->encryption.enable = false;
	conf->encryption.key    = "0123456789012345";
	conf->encryption.key_id = "kf";
	conf->encryption.type   = AES_GCM_V1;
	clear_folder(conf->dir);

	return conf;
}

void
conf_parquet_free(conf_parquet *conf)
{
	if (conf) {
		nng_strfree(conf->dir);
		nng_strfree(conf->file_name_prefix);
		nng_free(conf, sizeof(conf_parquet));
	}

	return;
}

void
parquet_write_batch_async_test(void)
{
	work **works = NULL;
	cvector_push_back(works, parquet_write_batch_async_test1());
	cvector_push_back(works, parquet_write_batch_async_test2());
	cvector_push_back(works, parquet_write_batch_async_test3());
	// cvector_push_back(works, parquet_write_batch_async_test4());
	// cvector_push_back(works, parquet_write_batch_async_test5());

	nng_msleep(100);
	works_free(works);
}

// void
// parquet_write_batch_async_tmp_test(void)
// {
// 	work **works = NULL;
// 	cvector_push_back(works, parquet_write_batch_tmp_async_test1());
// 	cvector_push_back(works, parquet_write_batch_tmp_async_test2());
// 
// 	nng_msleep(100);
// 	works_free(works);
// }

void
parquet_find_span_test()
{

	char *value = (char *) parquet_find(4000);
	check_mem(value);
	check_name(value, filenames[4]);
	nng_strfree(value);

	// Test normal case
	uint32_t size  = 0;
	char   **array = (char **) parquet_find_span(0, 4000, &size);
	check_mem(array);
	for (uint32_t i = 0; i < size; i++) {
		if (array[i]) {
			check_mem(array[i]);
			check_name(array[i], filenames[i]);
			nng_strfree(array[i]);
		}
	}
	check(size == 5, "find span size error");
	nng_free(array, size);

	// Test illegal case
	array = (char **) parquet_find_span(4000, 100, &size);
	check(array == NULL, "find span error");
	check(size == 0, "find span size error");

	array = (char **) parquet_find_span(5000, 8000, &size);
	check(size == 0, "find span size error");
	for (uint32_t i = 0; i < size; i++) {
		if (array[i]) {
			check_mem(array[i]);
			check_str(array[i], filenames[i]);
			nng_strfree(array[i]);
		}
	}

	nng_free(array, size);

	return;

error:
	puts("parquet_find_span_test failed!");
	abort();
}

void
parquet_find_data_packet_test()
{
	parquet_data_packet *pack = parquet_find_data_packet(
	    NULL, full_filenames[1], 1109);
	check_mem(pack);
	check(pack->size == strlen("hello world9"), "size error");
	check_nstr(pack->data, "hello world9", pack->size);
	FREE_STRUCT(pack->data);
	FREE_STRUCT(pack);

	parquet_data_packet **packs = parquet_find_data_packets(
	    NULL, full_filenames, find_keys_test, NUM_KEYS);
	check_mem(packs);
	for (int i = 0; i < NUM_KEYS; i++) {
		if (packs[i]) {
			check(packs[i]->size == strlen("hello world0"),
			    "size error");
			check_nstr(
			    packs[i]->data, "hello world0", packs[i]->size);
			FREE_STRUCT(packs[i]->data);
			FREE_STRUCT(packs[i]);
		}
	}
	free(packs);

	return;

error:
	puts("parquet_find_data_packet_test failed!");
	abort();
}

void
parquet_find_file_range_test()
{
	parquet_filename_range **file_ranges = parquet_find_file_range(10, 4409, "canudp");
	parquet_filename_range **file_ranges_for_free = file_ranges;
	check_mem(file_ranges);
	while (*file_ranges)
	{
		uint32_t size = 0;
		parquet_data_packet **packs = parquet_find_data_span_packets_specify_file(NULL, *file_ranges, &size);
		for (uint32_t i = 0; i < size; i++) {
			parquet_data_packet* pack = packs[i];
			char expect[32] = { 0 };
			sprintf(expect, "hello world%d", i);
			check(pack->size == strlen(expect), "size error");
		 	check_nstr(pack->data, expect, pack->size);
	 		FREE_STRUCT(packs[i]->data);
	 		FREE_STRUCT(packs[i]);
		}
		FREE_STRUCT(packs);
		nng_strfree((char*)(*file_ranges)->filename);
		FREE_STRUCT(*file_ranges);
		file_ranges++;
	}
	FREE_STRUCT(file_ranges_for_free);
	return;

error:
	puts("parquet_find_file_range_test failed!");
	abort();
}

void
parquet_get_key_span_test()
{
	uint64_t *data_span = NULL;
	data_span = parquet_get_key_span();
	check(data_span[0] == 10, "start key error");
	check(data_span[1] == 4409, "end key error");
	return;
error:
	puts("parquet_get_key_span_test failed!");
    abort();
}


int
main(int argc, char **argv)
{

	conf_parquet *conf = conf_parquet_init();

	parquet_write_launcher(conf);
	parquet_write_batch_async_test();
	puts("parquet write batch async passed!");
	// parquet_write_batch_async_tmp_test();
	// puts("parquet write batch tmp async passed!");
	// parquet_find_span_test();
	// puts("parquet_find_span_test passed!");
	// parquet_find_data_packet_test();
	// puts("parquet_find_data_packet_test passed!");
	// parquet_find_file_range_test();
	// puts("parquet_find_file_range_test passed!");
	// parquet_get_key_span_test();
	// puts("parquet_get_key_span_test passed!");

	// sleep(100000);

	return 0;
}
