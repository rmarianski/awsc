#ifndef AWSC_H
#define AWSC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <memarena.h>

typedef struct {
    ma_arena *arena_aws;
    ma_arena *arena_curl;
    ma_arena *arena_client;
    void *aws_mem_mgr;
} awsc_mem;

typedef struct  {
    char *aws_access_key_id;
    char *aws_secret_access_key;
} awsc_creds;

enum awsc_region {
    AWSC_REGION_UNKNOWN,
    AWSC_REGION_US_EAST,
};

typedef struct  {
    enum awsc_region region;  // must be set
    awsc_creds *creds;  // can be null
} awsc_config;

enum awsc_client_type {
    AWSC_CLIENT_UNKNOWN,
    AWSC_CLIENT_SQS,
};

struct awsc_state;

typedef struct  {
    enum awsc_client_type type;
    awsc_config *config;
    struct awsc_state *state;
    void *aws_client_handle;
} awsc_client;

typedef struct awsc_client_entry {
    awsc_client *client;
    struct awsc_client_entry *next;
} awsc_client_entry;

typedef struct awsc_client_cfg_entry {
    void *aws_client_cfg;
    struct awsc_client_cfg_entry *next;
} awsc_client_cfg_entry;

typedef struct awsc_state {
    awsc_mem *mem;
    awsc_client_entry *clients;  // list of all aws clients
    awsc_client_cfg_entry *aws_client_cfgs;  // list of all aws client cfgs
    void *aws_sdk_options;  // Aws::SDKOptions
} awsc_state;

// this should be the first thing that happens
awsc_state *awsc_state_create(void *memory, size_t aws, size_t curl, size_t client);
// TODO possibly take options to control internal aws behavior
void awsc_state_init(awsc_state *state);
void awsc_state_destroy(awsc_state *state);

enum awsc_region awsc_str_to_region(char *region);

// creds can be null
awsc_config *awsc_create_config(awsc_state *state, enum awsc_region region, awsc_creds *creds);

awsc_client *awsc_client_find(awsc_state *state, enum awsc_client_type type, enum awsc_region region);

awsc_client *awsc_client_create_sqs(awsc_state *state, awsc_config *config);

typedef struct  {
    char *error;
    char *url;
} awsc_sqs_url_result;
awsc_sqs_url_result awsc_sqs_url(awsc_client *sqs_client, char *queue_name);

typedef struct  {
    char *error;
    unsigned long n;
} awsc_sqs_n_result;
awsc_sqs_n_result awsc_sqs_n(awsc_client *sqs_client, char *url);

#ifdef __cplusplus
}
#endif

#endif
