#ifndef AWSC_H
#define AWSC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <memarena.h>
#include <stdbool.h>

typedef struct {
    ma_ctx *arena_aws;
    ma_ctx *arena_curl;
    ma_ctx *arena_client;
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
    AWSC_CLIENT_OPSWORKS,
    AWSC_CLIENT_EC2,
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
    // TODO consider putting the actual values here instead of pointers
    // but maybe it's better to leave it opaque to not need SDKOptions?
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
awsc_client *awsc_client_create_ops(awsc_state *state, awsc_config *config);
awsc_client *awsc_client_create_ec2(awsc_state *state, awsc_config *config);

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

typedef struct {
    char *id;
    char *name;
    char *status;
} awsc_ops_instance;

typedef struct {
    char *error;
    awsc_ops_instance *instances;
    unsigned int n_instances;
} awsc_ops_list_result;
awsc_ops_list_result awsc_ops_list(awsc_client *ops_client, char *layer, char *stack, char *env);

typedef struct {
    char *instance_type;
    char *root_device;
    char *availability_zone;
    bool add_base_layer;
    awsc_client *ec2_client;
} awsc_ops_create_options;

typedef struct {
    char *error;
    char *id;
} awsc_ops_create_result;

awsc_ops_create_result awsc_ops_create(awsc_client *ops_client, char *layer, char *stack, char *env, awsc_ops_create_options *options);

#ifdef __cplusplus
}
#endif

#endif
