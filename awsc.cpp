#include <stdio.h>
#include <string.h>
#include <memarena.h>

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/curl/CurlHttpClient.h>

#include <aws/sqs/SQSClient.h>
#include <aws/sqs/model/GetQueueUrlRequest.h>
#include <aws/sqs/model/GetQueueUrlResult.h>
#include <aws/sqs/model/GetQueueAttributesRequest.h>
#include <aws/sqs/model/GetQueueAttributesResult.h>
#include <aws/sqs/model/QueueAttributeName.h>

#include <new>

#include "awsc.h"
#include "memmgr.h"

awsc_state *awsc_state_create(void *memory, size_t aws, size_t curl, size_t client) {
    unsigned char *cur_loc = (unsigned char *)memory;

    // allocate space for internal structures
    awsc_state *state = (awsc_state *)cur_loc;
    cur_loc += sizeof(awsc_state);

    awsc_mem *aws_mem = (awsc_mem *)cur_loc;
    cur_loc += sizeof(awsc_mem);

    ma_arena *arena_aws = (ma_arena *)cur_loc;
    cur_loc += sizeof(ma_arena);

    ma_arena *arena_curl = (ma_arena *)cur_loc;
    cur_loc += sizeof(ma_arena);

    ma_arena *arena_client = (ma_arena *)cur_loc;
    cur_loc += sizeof(ma_arena);

    void *mem_mgr_loc = cur_loc;
    cur_loc += sizeof(CustomMemoryManager);

    void *mem_options = cur_loc;
    cur_loc += sizeof(Aws::SDKOptions);

    // subtract internal structures space from aws arena
    unsigned int internal_size = (uintptr_t)cur_loc - (uintptr_t)memory;
    aws -= internal_size;

    ma_init(cur_loc, aws, arena_aws);
    cur_loc += aws;

    ma_init(cur_loc, curl, arena_curl);
    cur_loc += curl;

    ma_init(cur_loc, client, arena_client);
    cur_loc += client;

    aws_mem->arena_aws = arena_aws;
    aws_mem->arena_curl = arena_curl;
    aws_mem->arena_client = arena_client;
    aws_mem->aws_mem_mgr = mem_mgr_loc;

    CustomMemoryManager *aws_mem_mgr = new(mem_mgr_loc) CustomMemoryManager(arena_aws, arena_curl);

    Aws::SDKOptions *options = new(mem_options) Aws::SDKOptions();
    options->memoryManagementOptions.memoryManager = aws_mem_mgr;

    state->mem = aws_mem;
    state->clients = NULL;
    state->aws_client_cfgs = NULL;
    state->aws_sdk_options = options;

    return state;
}

void awsc_state_init(awsc_state *state) {
    Aws::SDKOptions *options = (Aws::SDKOptions *)state->aws_sdk_options;
    Aws::InitAPI(*options);
}

void awsc_state_destroy(awsc_state *state) {
    for (awsc_client_entry *entry = state->clients;
         entry;
         entry = entry->next) {
        awsc_client *client = entry->client;
        switch (client->type) {
        case AWSC_CLIENT_SQS: {
            Aws::SQS::SQSClient *sqsClient = (Aws::SQS::SQSClient *)client->aws_client_handle;
            sqsClient->~SQSClient();
            break;
        }
        case AWSC_CLIENT_UNKNOWN:
            assert(0 && "Found awsc client unknown");
        default:
            assert(0 && "Unknown client type");
        }
    }

    for (awsc_client_cfg_entry *entry = state->aws_client_cfgs;
         entry;
         entry = entry->next) {
        Aws::Client::ClientConfiguration *cfg = (Aws::Client::ClientConfiguration *)entry->aws_client_cfg;
        cfg->~ClientConfiguration();
    }

    Aws::SDKOptions *options = (Aws::SDKOptions *)state->aws_sdk_options;
    Aws::ShutdownAPI(*options);
}

awsc_region awsc_str_to_region(char *region) {
    if (strcmp(region, "us-east-1") == 0)
        return AWSC_REGION_US_EAST;
    else
        return AWSC_REGION_UNKNOWN;
}

awsc_config *awsc_create_config(awsc_state *state, awsc_region region, awsc_creds *creds) {
    awsc_config *result = (awsc_config *)ma_push_struct(state->mem->arena_aws, awsc_config);
    result->region = region;
    result->creds = creds;
    return result;
}

static Aws::Client::ClientConfiguration *create_aws_client_cfg(awsc_state *state, awsc_config *config) {
    void *mem_client_cfg = ma_push_struct(state->mem->arena_aws, Aws::Client::ClientConfiguration);
    Aws::Client::ClientConfiguration *clientConfiguration = new(mem_client_cfg) Aws::Client::ClientConfiguration;
    switch (config->region) {
    case AWSC_REGION_US_EAST:
        clientConfiguration->region = Aws::Region::US_EAST_1;
        break;
    default:
        assert(0 && "Unknown region");
    }

    awsc_client_cfg_entry *entry = (awsc_client_cfg_entry *)ma_push_struct(state->mem->arena_aws, awsc_client_cfg_entry);
    entry->aws_client_cfg = clientConfiguration;
    entry->next = state->aws_client_cfgs;
    state->aws_client_cfgs = entry;

    return clientConfiguration;
}

awsc_client *awsc_client_find(awsc_state *state, awsc_client_type type, awsc_region region) {
    for (awsc_client_entry *entry = state->clients; entry; entry = entry->next)
        if (entry->client->type == type && entry->client->config->region == region)
            return entry->client;
    return NULL;
}

awsc_client *awsc_client_create_sqs(awsc_state *state, awsc_config *config) {
    awsc_client *result = awsc_client_find(state, AWSC_CLIENT_SQS, config->region);
    if (result)
        return result;
    result = (awsc_client *)ma_push_struct(state->mem->arena_aws, awsc_client);

    Aws::Client::ClientConfiguration *aws_client_cfg = create_aws_client_cfg(state, config);
    void *mem_sqs_client = ma_push_struct(state->mem->arena_aws, Aws::SQS::SQSClient);
    Aws::SQS::SQSClient *sqsClient = new(mem_sqs_client) Aws::SQS::SQSClient(*aws_client_cfg);

    result = (awsc_client *)ma_push_struct(state->mem->arena_aws, awsc_client);
    result->type = AWSC_CLIENT_SQS;
    result->config = config;
    result->state = state;
    result->aws_client_handle = sqsClient;

    awsc_client_entry *entry = (awsc_client_entry *)ma_push_struct(state->mem->arena_aws, awsc_client_entry);
    entry->client = result;
    entry->next = state->clients;
    state->clients = entry;

    return result;
}

awsc_sqs_url_result awsc_sqs_url(awsc_client *sqs_client, char *queue_name) {
    awsc_sqs_url_result result = {};
    awsc_state *state = sqs_client->state;
    ma_snapshot *snapshot = ma_snapshot_save(state->mem->arena_aws);

    Aws::SQS::SQSClient *sqsClient = (Aws::SQS::SQSClient *)sqs_client->aws_client_handle;

    Aws::SQS::Model::GetQueueUrlRequest urlReq;
    urlReq.SetQueueName(queue_name);
    auto getQueueUrlOutcome = sqsClient->GetQueueUrl(urlReq);
    if (!getQueueUrlOutcome.IsSuccess()) {
        auto error = getQueueUrlOutcome.GetError();
        const Aws::String& message = error.GetMessage();
        result.error = (char *)ma_push(state->mem->arena_client, message.size());
        strcpy(result.error, message.c_str());
        goto snapshot_restore;
    } else {
        Aws::SQS::Model::GetQueueUrlResult& getQueueUrlResult = getQueueUrlOutcome.GetResult();
        const Aws::String& urlQueueString = getQueueUrlResult.GetQueueUrl();
        result.url = (char *)ma_push(state->mem->arena_client, urlQueueString.size());
        strcpy(result.url, urlQueueString.c_str());
    }

snapshot_restore:
    ma_snapshot_restore(snapshot);

    return result;
}

awsc_sqs_n_result awsc_sqs_n(awsc_client *sqs_client, char *url) {
    awsc_sqs_n_result result = {};
    awsc_state *state = sqs_client->state;
    ma_snapshot *snapshot = ma_snapshot_save(state->mem->arena_aws);

    Aws::SQS::SQSClient *sqsClient = (Aws::SQS::SQSClient *)sqs_client->aws_client_handle;

    Aws::SQS::Model::GetQueueAttributesRequest getAttrReq;
    getAttrReq.SetQueueUrl(url);
    Aws::Vector<Aws::SQS::Model::QueueAttributeName> attrs;
    attrs.push_back(Aws::SQS::Model::QueueAttributeName::ApproximateNumberOfMessages);
    getAttrReq.SetAttributeNames(attrs);
    auto getAttrsOutcome = sqsClient->GetQueueAttributes(getAttrReq);
    if (!getAttrsOutcome.IsSuccess()) {
        auto error = getAttrsOutcome.GetError();
        const Aws::String& message = error.GetMessage();
        result.error = (char *)ma_push(state->mem->arena_client, message.size());
        strcpy(result.error, message.c_str());
        goto snapshot_restore;
    } else {
        Aws::SQS::Model::GetQueueAttributesResult& getQueueAttrsResult = getAttrsOutcome.GetResult();
        const Aws::Map<Aws::SQS::Model::QueueAttributeName, Aws::String>& attrResults = getQueueAttrsResult.GetAttributes();
        auto nMsgsStrIt = attrResults.find(Aws::SQS::Model::QueueAttributeName::ApproximateNumberOfMessages);
        if (nMsgsStrIt == attrResults.end()) {
            result.error = "SQS::GetQueueAttributes did not contain approximate number of messages";
            goto snapshot_restore;
        } else {
            Aws::String nMsgsStr = nMsgsStrIt->second;
            result.n = atol(nMsgsStr.c_str());
        }
    }

snapshot_restore:
    ma_snapshot_restore(snapshot);

    return result;
}
