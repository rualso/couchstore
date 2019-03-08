/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

/**
 * @copyright 2013 Couchbase, Inc.
 *
 * @author Filipe Manana  <filipe@couchbase.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 **/

#include "src/views/mapreduce/mapreduce.h"
#include <platform/cb_malloc.h>
#include <platform/cbassert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __STDC_VERSION__ >=199901L
#define ASSIGN(x) x =
#else
#define ASSIGN(x)
#endif

static const mapreduce_json_t key1 = {
    ASSIGN(.json) "[1,1]",
    ASSIGN(.length) sizeof("[1,1]") - 1
};
static const mapreduce_json_t value1 = {
    ASSIGN(.json) "11",
    ASSIGN(.length) sizeof("11") - 1
};

static const mapreduce_json_t key2 = {
    ASSIGN(.json) "[2,2]",
    ASSIGN(.length) sizeof("[2,2]") - 1
};
static const mapreduce_json_t value2 = {
    ASSIGN(.json) "22",
    ASSIGN(.length) sizeof("22") - 1
};

static const mapreduce_json_t key3 = {
    ASSIGN(.json) "[3,3]",
    ASSIGN(.length) sizeof("[3,3]") - 1
};
static const mapreduce_json_t value3 = {
    ASSIGN(.json) "33",
    ASSIGN(.length) sizeof("33") - 1
};

static const mapreduce_json_t key4 = {
    ASSIGN(.json) "[4,4]",
    ASSIGN(.length) sizeof("[4,4]") - 1
};
static const mapreduce_json_t value4 = {
    ASSIGN(.json) "44",
    ASSIGN(.length) sizeof("44") - 1
};

static mapreduce_json_list_t *all_keys(void);
static mapreduce_json_list_t *all_values(void);
static void free_json_list(mapreduce_json_list_t *list);

static void test_bad_syntax_functions(void)
{
    void *context = NULL;
    char *error_msg = NULL;
    mapreduce_error_t ret;
    const char *functions[] = {
        "function(key, values, rereduce) { return values.length; }",
        "function(key, values, rereduce) { return values.length * 2;"
    };

    ret = mapreduce_start_reduce_context(functions, 2, &context, &error_msg);
    cb_assert(ret == MAPREDUCE_SYNTAX_ERROR);
    cb_assert(error_msg != NULL);
    cb_assert(strlen(error_msg) > 0);
    cb_assert(context == NULL);

    mapreduce_free_error_msg(error_msg);
}

static void test_runtime_exception(void)
{
    void *context = NULL;
    char *error_msg = NULL;
    mapreduce_error_t ret;
    const char *functions[] = {
        "function(key, values, rereduce) { throw('foobar'); }"
    };
    mapreduce_json_list_t *result = NULL;
    mapreduce_json_list_t *keys;
    mapreduce_json_list_t *values;

    ret = mapreduce_start_reduce_context(functions, 1, &context, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(context != NULL);

    keys = all_keys();
    values = all_values();

    ret = mapreduce_reduce_all(context, keys, values, &result, &error_msg);
    cb_assert(ret == MAPREDUCE_RUNTIME_ERROR);
    cb_assert(result == NULL);
    cb_assert(error_msg != NULL);

    mapreduce_free_error_msg(error_msg);
    mapreduce_free_context(context);
    free_json_list(keys);
    free_json_list(values);
}

static void test_runtime_error(void)
{
    void *context = NULL;
    char *error_msg = NULL;
    mapreduce_error_t ret;
    const char *functions[] = {
        "function(key, values, rereduce) { return sum(values); }",
        "function(key, values, rereduce) { return values[0].foo.bar; }"
    };
    mapreduce_json_list_t *result = NULL;
    mapreduce_json_list_t *keys;
    mapreduce_json_list_t *values;
    mapreduce_json_t *reduction = NULL;

    ret = mapreduce_start_reduce_context(functions, 2, &context, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(context != NULL);

    keys = all_keys();
    values = all_values();

    /* reduce all */
    ret = mapreduce_reduce_all(context, keys, values, &result, &error_msg);
    cb_assert(ret == MAPREDUCE_RUNTIME_ERROR);
    cb_assert(result == NULL);
    cb_assert(error_msg != NULL);
    cb_assert(strcmp(
        "TypeError: Cannot read property 'bar' of undefined (line 1:55)",
        error_msg) == 0);

    mapreduce_free_error_msg(error_msg);
    error_msg = NULL;

    /* reduce single function (2nd) */

    ret = mapreduce_reduce(context, 2, keys, values, &reduction, &error_msg);
    cb_assert(ret == MAPREDUCE_RUNTIME_ERROR);
    cb_assert(reduction == NULL);
    cb_assert(error_msg != NULL);
    cb_assert(strcmp(
        "TypeError: Cannot read property 'bar' of undefined (line 1:55)",
        error_msg) == 0);

    mapreduce_free_error_msg(error_msg);

    /* reduce single function (1st), should succeed */

    ret = mapreduce_reduce(context, 1, keys, values, &reduction, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(reduction != NULL);
    cb_assert(error_msg == NULL);
    cb_assert(reduction->length == (sizeof("110") - 1));
    cb_assert(strncmp(reduction->json, "110", sizeof("110") - 1) == 0);

    mapreduce_free_json(reduction);

    mapreduce_free_context(context);
    free_json_list(keys);
    free_json_list(values);
}

static void test_reduce_emits(void)
{
    void *context = NULL;
    char *error_msg = NULL;
    mapreduce_error_t ret;
    const char *functions[] = {
        "function(key, values, rereduce) { emit(key, values); return sum(values); }"
    };
    mapreduce_json_list_t *result = NULL;
    mapreduce_json_list_t *keys;
    mapreduce_json_list_t *values;

    ret = mapreduce_start_reduce_context(functions, 1, &context, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(context != NULL);

    keys = all_keys();
    values = all_values();

    ret = mapreduce_reduce_all(context, keys, values, &result, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(result != NULL);
    cb_assert(result->length == 1);
    cb_assert(result->values[0].length == (sizeof("110") - 1));
    cb_assert(strncmp("110", result->values[0].json, sizeof("110") - 1) == 0);

    mapreduce_free_json_list(result);
    mapreduce_free_context(context);
    free_json_list(keys);
    free_json_list(values);
}


static void test_reduce_and_rereduce_success(void)
{
    void *context = NULL;
    char *error_msg = NULL;
    mapreduce_error_t ret;
    const char *functions[] = {
        "function(key, values, rereduce) {"
        "  if (rereduce) {"
        "    return sum(values);"
        "  } else {"
        "    return values.length;"
        "  }"
        "}",
        "function(key, values, rereduce) {"
        "  if (rereduce) {"
        "    return values[values.length - 1];"
        "  } else {"
        "    return values[0];"
        "  }"
        "}"
    };
    mapreduce_json_list_t *result = NULL;
    mapreduce_json_list_t *keys = NULL;
    mapreduce_json_list_t *values = NULL;
    mapreduce_json_t *reduction = NULL;

    ret = mapreduce_start_reduce_context(functions, 2, &context, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(context != NULL);

    keys = all_keys();
    values = all_values();

    /* reduce all */
    ret = mapreduce_reduce_all(context, keys, values, &result, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(result != NULL);
    cb_assert(result->length == 2);
    cb_assert(result->values[0].length == (sizeof("4") - 1));
    cb_assert(strncmp(result->values[0].json, "4", (sizeof("4") - 1)) == 0);
    cb_assert(result->values[1].length == (sizeof("11") - 1));
    cb_assert(strncmp(result->values[1].json, "11", (sizeof("11") - 1)) == 0);

    mapreduce_free_json_list(result);

    /* reduce single function (1st) */

    ret = mapreduce_reduce(context, 1, keys, values, &reduction, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(reduction != NULL);
    cb_assert(reduction->length == (sizeof("4") - 1));
    cb_assert(strncmp(reduction->json, "4", sizeof("4") - 1) == 0);

    mapreduce_free_json(reduction);
    reduction = NULL;

    /* reduce single function (2nd), should succeed */

    ret = mapreduce_reduce(context, 2, keys, values, &reduction, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(reduction != NULL);
    cb_assert(error_msg == NULL);
    cb_assert(reduction->length == (sizeof("11") - 1));
    cb_assert(strncmp(reduction->json, "11", sizeof("11") - 1) == 0);

    mapreduce_free_json(reduction);
    reduction = NULL;

    /* rereduce, 1st function */

    ret = mapreduce_rereduce(context, 1, values, &reduction, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(reduction != NULL);
    cb_assert(reduction->length == (sizeof("110") - 1));
    cb_assert(strncmp(reduction->json, "110", sizeof("110") - 1) == 0);

    mapreduce_free_json(reduction);
    reduction = NULL;

    /* rereduce, 2nd function */

    ret = mapreduce_rereduce(context, 2, values, &reduction, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(reduction != NULL);
    cb_assert(reduction->length == (sizeof("44") - 1));
    cb_assert(strncmp(reduction->json, "44", sizeof("44") - 1) == 0);

    mapreduce_free_json(reduction);

    mapreduce_free_context(context);
    free_json_list(keys);
    free_json_list(values);
}

static void test_timeout(void)
{
    void *context = NULL;
    char *error_msg = NULL;
    mapreduce_error_t ret;
    const char *functions[] = {
        "function(key, values, rereduce) {"
        "  if (rereduce) {"
        "    return sum(values);"
        "  } else {"
        "    while (true) { };"
        "    return values.length;"
        "  }"
        "}"
    };
    mapreduce_json_list_t *result = NULL;
    mapreduce_json_list_t *keys = NULL;
    mapreduce_json_list_t *values = NULL;
    mapreduce_json_t *reduction = NULL;

    ret = mapreduce_start_reduce_context(functions, 1, &context, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(context != NULL);

    keys = all_keys();
    values = all_values();

    /* reduce all */
    ret = mapreduce_reduce_all(context, keys, values, &result, &error_msg);
    cb_assert(ret == MAPREDUCE_TIMEOUT);
    cb_assert(result == NULL);
    cb_assert(error_msg != NULL);
    cb_assert(strcmp(error_msg, "timeout") == 0);

    mapreduce_free_error_msg(error_msg);
    error_msg = NULL;

    /* rereduce, 1st function */
    ret = mapreduce_rereduce(context, 1, values, &reduction, &error_msg);
    cb_assert(ret == MAPREDUCE_SUCCESS);
    cb_assert(error_msg == NULL);
    cb_assert(reduction != NULL);
    cb_assert(reduction->length == (sizeof("110") - 1));
    cb_assert(strncmp(reduction->json, "110", sizeof("110") - 1) == 0);

    mapreduce_free_json(reduction);
    mapreduce_free_context(context);
    free_json_list(keys);
    free_json_list(values);
}

static mapreduce_json_list_t *all_keys(void)
{
    mapreduce_json_list_t *ret = (mapreduce_json_list_t *) cb_malloc(sizeof(*ret));

    cb_assert(ret != NULL);
    ret->length = 4;
    ret->values = (mapreduce_json_t *) cb_malloc(sizeof(mapreduce_json_t) * ret->length);
    cb_assert(ret->values != NULL);
    ret->values[0] = key1;
    ret->values[1] = key2;
    ret->values[2] = key3;
    ret->values[3] = key4;

    return ret;
}

static mapreduce_json_list_t *all_values(void)
{
    mapreduce_json_list_t *ret = (mapreduce_json_list_t *) cb_malloc(sizeof(*ret));

    cb_assert(ret != NULL);
    ret->length = 4;
    ret->values = (mapreduce_json_t *) cb_malloc(sizeof(mapreduce_json_t) * ret->length);
    cb_assert(ret->values != NULL);
    ret->values[0] = value1;
    ret->values[1] = value2;
    ret->values[2] = value3;
    ret->values[3] = value4;

    return ret;
}

static void free_json_list(mapreduce_json_list_t *list)
{
    cb_free(list->values);
    cb_free(list);
}

int main(void)
{
    fprintf(stderr, "Running reduce tests\n");
    mapreduce_init();

    mapreduce_set_timeout(1);
    test_timeout();

    test_bad_syntax_functions();
    test_runtime_exception();
    test_runtime_error();
    test_reduce_emits();
    test_reduce_and_rereduce_success();

    test_timeout();
    mapreduce_deinit();
    return 0;
}
