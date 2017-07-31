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

#include <aws/opsworks/OpsWorksClient.h>
#include <aws/opsworks/model/DescribeStacksRequest.h>
#include <aws/opsworks/model/DescribeLayersRequest.h>
#include <aws/opsworks/model/DescribeInstancesRequest.h>
#include <aws/opsworks/model/CreateInstanceRequest.h>
#include <aws/opsworks/model/CreateInstanceResult.h>
#include <aws/opsworks/model/GetHostnameSuggestionRequest.h>
#include <aws/opsworks/model/GetHostnameSuggestionResult.h>
#include <aws/opsworks/model/RootDeviceType.h>

#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/DescribeVpcsRequest.h>
#include <aws/ec2/model/DescribeVpcsResponse.h>
#include <aws/ec2/model/DescribeSubnetsRequest.h>
#include <aws/ec2/model/DescribeSubnetsResponse.h>

#include <new>

#include "awsc.h"
#include "memmgr.h"

awsc_state *awsc_state_create(void *memory, size_t aws, size_t curl, size_t client) {
    // TODO need to update this
    ma_ctx_linear alloc_linear;
    ma_ctx *linear = ma_init_linear(&alloc_linear, memory, aws + curl + client);

    // allocate space for internal structures
    awsc_state *state = (awsc_state *)ma_alloc_struct(linear, awsc_state);
    awsc_mem *aws_mem = (awsc_mem *)ma_alloc_struct(linear, awsc_mem);

    void *aws_mem_addr = ma_alloc(linear, aws + 256);
    ma_ctx *arena_aws = ma_create_allocator_linear(aws_mem_addr, aws);

    void *curl_mem_addr = ma_alloc(linear, curl + 256);
    ma_ctx *arena_curl = ma_create_allocator_freelist(curl_mem_addr, curl);

    void *client_mem_addr = ma_alloc(linear, client + 256);
    ma_ctx *arena_client = ma_create_allocator_linear(client_mem_addr, client);

    void *mem_mgr_loc = ma_alloc_struct(linear, CustomMemoryManager);
    void *mem_options = ma_alloc_struct(linear, Aws::SDKOptions);

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
        case AWSC_CLIENT_OPSWORKS: {
            Aws::OpsWorks::OpsWorksClient *opsClient = (Aws::OpsWorks::OpsWorksClient *)client->aws_client_handle;
            opsClient->~OpsWorksClient();
            break;
        }
        case AWSC_CLIENT_EC2: {
            Aws::EC2::EC2Client *ec2Client = (Aws::EC2::EC2Client *)client->aws_client_handle;
            ec2Client->~EC2Client();
            break;
        }
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
    awsc_config *result = (awsc_config *)ma_alloc_struct(state->mem->arena_aws, awsc_config);
    result->region = region;
    result->creds = creds;
    return result;
}

static Aws::Client::ClientConfiguration *create_aws_client_cfg(awsc_state *state, awsc_config *config) {
    void *mem_client_cfg = ma_alloc_struct(state->mem->arena_aws, Aws::Client::ClientConfiguration);
    Aws::Client::ClientConfiguration *clientConfiguration = new(mem_client_cfg) Aws::Client::ClientConfiguration;
    switch (config->region) {
    case AWSC_REGION_US_EAST:
        clientConfiguration->region = Aws::Region::US_EAST_1;
        break;
    default:
        assert(0 && "Unknown region");
    }

    awsc_client_cfg_entry *entry = (awsc_client_cfg_entry *)ma_alloc_struct(state->mem->arena_aws, awsc_client_cfg_entry);
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
    result = (awsc_client *)ma_alloc_struct(state->mem->arena_aws, awsc_client);

    Aws::Client::ClientConfiguration *aws_client_cfg = create_aws_client_cfg(state, config);
    void *mem_sqs_client = ma_alloc_struct(state->mem->arena_aws, Aws::SQS::SQSClient);
    Aws::SQS::SQSClient *sqsClient = new(mem_sqs_client) Aws::SQS::SQSClient(*aws_client_cfg);

    result = (awsc_client *)ma_alloc_struct(state->mem->arena_aws, awsc_client);
    result->type = AWSC_CLIENT_SQS;
    result->config = config;
    result->state = state;
    result->aws_client_handle = sqsClient;

    awsc_client_entry *entry = (awsc_client_entry *)ma_alloc_struct(state->mem->arena_aws, awsc_client_entry);
    entry->client = result;
    entry->next = state->clients;
    state->clients = entry;

    return result;
}

awsc_sqs_url_result awsc_sqs_url(awsc_client *sqs_client, char *queue_name) {
    awsc_sqs_url_result result = {};
    awsc_state *state = sqs_client->state;
    ma_linear_snapshot *snapshot = ma_snapshot_save(state->mem->arena_aws);

    Aws::SQS::SQSClient *sqsClient = (Aws::SQS::SQSClient *)sqs_client->aws_client_handle;

    Aws::SQS::Model::GetQueueUrlRequest urlReq;
    urlReq.SetQueueName(queue_name);
    auto getQueueUrlOutcome = sqsClient->GetQueueUrl(urlReq);
    if (!getQueueUrlOutcome.IsSuccess()) {
        auto error = getQueueUrlOutcome.GetError();
        const Aws::String& message = error.GetMessage();
        result.error = (char *)ma_alloc(state->mem->arena_client, message.size() + 1);
        strcpy(result.error, message.c_str());
        goto snapshot_restore;
    } else {
        Aws::SQS::Model::GetQueueUrlResult& getQueueUrlResult = getQueueUrlOutcome.GetResult();
        const Aws::String& urlQueueString = getQueueUrlResult.GetQueueUrl();
        result.url = (char *)ma_alloc(state->mem->arena_client, urlQueueString.size() + 1);
        strcpy(result.url, urlQueueString.c_str());
    }

snapshot_restore:
    ma_snapshot_restore(snapshot);

    return result;
}

awsc_sqs_n_result awsc_sqs_n(awsc_client *sqs_client, char *url) {
    awsc_sqs_n_result result = {};
    awsc_state *state = sqs_client->state;
    ma_linear_snapshot *snapshot = ma_snapshot_save(state->mem->arena_aws);

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
        result.error = (char *)ma_alloc(state->mem->arena_client, message.size() + 1);
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

awsc_client *awsc_client_create_ops(awsc_state *state, awsc_config *config) {
    awsc_client *result = awsc_client_find(state, AWSC_CLIENT_OPSWORKS, config->region);
    if (result)
        return result;
    result = (awsc_client *)ma_alloc_struct(state->mem->arena_aws, awsc_client);

    Aws::Client::ClientConfiguration *aws_client_cfg = create_aws_client_cfg(state, config);
    void *mem_ops_client = ma_alloc_struct(state->mem->arena_aws, Aws::OpsWorks::OpsWorksClient);
    Aws::OpsWorks::OpsWorksClient *opsClient = new(mem_ops_client) Aws::OpsWorks::OpsWorksClient(*aws_client_cfg);

    result = (awsc_client *)ma_alloc_struct(state->mem->arena_aws, awsc_client);
    result->type = AWSC_CLIENT_OPSWORKS;
    result->config = config;
    result->state = state;
    result->aws_client_handle = opsClient;

    awsc_client_entry *entry = (awsc_client_entry *)ma_alloc_struct(state->mem->arena_aws, awsc_client_entry);
    entry->client = result;
    entry->next = state->clients;
    state->clients = entry;

    return result;
}

awsc_client *awsc_client_create_ec2(awsc_state *state, awsc_config *config) {
    awsc_client *result = awsc_client_find(state, AWSC_CLIENT_EC2, config->region);
    if (result)
        return result;
    result = (awsc_client *)ma_alloc_struct(state->mem->arena_aws, awsc_client);

    Aws::Client::ClientConfiguration *aws_client_cfg = create_aws_client_cfg(state, config);
    void *mem_ec2_client = ma_alloc_struct(state->mem->arena_aws, Aws::EC2::EC2Client);
    Aws::EC2::EC2Client *ec2Client = new(mem_ec2_client) Aws::EC2::EC2Client(*aws_client_cfg);

    result = (awsc_client *)ma_alloc_struct(state->mem->arena_aws, awsc_client);
    result->type = AWSC_CLIENT_EC2;
    result->config = config;
    result->state = state;
    result->aws_client_handle = ec2Client;

    awsc_client_entry *entry = (awsc_client_entry *)ma_alloc_struct(state->mem->arena_aws, awsc_client_entry);
    entry->client = result;
    entry->next = state->clients;
    state->clients = entry;

    return result;
}

typedef struct {
    char *error;
    char *value;
} maybe_string;

static maybe_string lookup_stack_id(Aws::OpsWorks::OpsWorksClient *opsClient, char *stack_name, ma_ctx *arena) {
    maybe_string result = {};
    Aws::OpsWorks::Model::DescribeStacksRequest stacksReq;
    auto stacksOutcome = opsClient->DescribeStacks(stacksReq);
    if (!stacksOutcome.IsSuccess()) {
        auto error = stacksOutcome.GetError();
        const Aws::String& message = error.GetMessage();
        result.error = (char *)ma_alloc(arena, message.size() + 1);
        strcpy(result.error, message.c_str());
    } else {
        Aws::OpsWorks::Model::DescribeStacksResult& stacksResult = stacksOutcome.GetResult();
        const Aws::Vector<Aws::OpsWorks::Model::Stack>& stacks = stacksResult.GetStacks();
        for (auto stack: stacks) {
            const Aws::String& stackNameStr = stack.GetName();
            if (strcmp(stack_name, stackNameStr.c_str()) == 0) {
                const Aws::String& awsStackId = stack.GetStackId();
                result.value = (char *)ma_alloc(arena, awsStackId.size() + 1);
                strcpy(result.value, awsStackId.c_str());
                break;
            }
        }
        if (!result.value) {
            result.error = "Could not find stack";
        }
    }
    return result;
}

static maybe_string lookup_layer_id(Aws::OpsWorks::OpsWorksClient *opsClient, char *stack_id, char *layer_name, ma_ctx *arena) {
    maybe_string result = {};

    Aws::OpsWorks::Model::DescribeLayersRequest layersReq;
    layersReq.SetStackId(stack_id);
    auto layersOutcome = opsClient->DescribeLayers(layersReq);
    if (!layersOutcome.IsSuccess()) {
        auto error = layersOutcome.GetError();
        const Aws::String& message = error.GetMessage();
        result.error = (char *)ma_alloc(arena, message.size() + 1);
        strcpy(result.error, message.c_str());
    } else {
        Aws::OpsWorks::Model::DescribeLayersResult& layersResult = layersOutcome.GetResult();
        const Aws::Vector<Aws::OpsWorks::Model::Layer> layers = layersResult.GetLayers();
        for (auto layer: layers) {
            const Aws::String& layerNameStr = layer.GetShortname();
            if (strcmp(layer_name, layerNameStr.c_str()) == 0) {
                const Aws::String& awsLayerId = layer.GetLayerId();
                result.value = (char *)ma_alloc(arena, awsLayerId.size() + 1);
                strcpy(result.value, awsLayerId.c_str());
                break;
            }
        }
    }
    if (!result.value) {
        result.error = "Could not find layer";
    }

    return result;
}

typedef struct {
    char *error;
    char *stack_id;
    char *layer_id;
} maybe_stack_and_layer_ids;

maybe_stack_and_layer_ids lookup_stack_and_layer_ids(Aws::OpsWorks::OpsWorksClient *opsClient, char *stack_name, char *layer_name, ma_ctx *arena) {
    maybe_stack_and_layer_ids result = {};
    maybe_string maybe_stackid = lookup_stack_id(opsClient, stack_name, arena);
    if (maybe_stackid.error) {
        result.error = maybe_stackid.error;
    } else {
        result.stack_id = maybe_stackid.value;
        maybe_string maybe_layerid = lookup_layer_id(opsClient, result.stack_id, layer_name, arena);
        if (maybe_layerid.error) {
            result.error = maybe_layerid.error;
        } else {
            result.layer_id = maybe_layerid.value;
        }
    }
    return result;
}

awsc_ops_list_result awsc_ops_list(awsc_client *ops_client, char *layer_name, char *stack_name, char *env_name) {
    awsc_ops_list_result result = {};
    awsc_state *state = ops_client->state;
    ma_linear_snapshot *snapshot = ma_snapshot_save(state->mem->arena_aws);

    Aws::OpsWorks::OpsWorksClient *opsClient = (Aws::OpsWorks::OpsWorksClient *)ops_client->aws_client_handle;

    maybe_stack_and_layer_ids stack_and_layer_ids = lookup_stack_and_layer_ids(opsClient, stack_name, layer_name, state->mem->arena_client);
    if (stack_and_layer_ids.error) {
        result.error = stack_and_layer_ids.error;
        goto snapshot_restore;
    } else {

        char *layer_id = stack_and_layer_ids.layer_id;
        Aws::OpsWorks::Model::DescribeInstancesRequest describeReq;
        describeReq.SetLayerId(layer_id);
        auto describeOutcome = opsClient->DescribeInstances(describeReq);
        if (!describeOutcome.IsSuccess()) {
            auto error = describeOutcome.GetError();
            const Aws::String& message = error.GetMessage();
            result.error = (char *)ma_alloc(state->mem->arena_client, message.size() + 1);
            strcpy(result.error, message.c_str());
            goto snapshot_restore;
        } else {
            Aws::OpsWorks::Model::DescribeInstancesResult& describeResult = describeOutcome.GetResult();
            const Aws::Vector<Aws::OpsWorks::Model::Instance>& awsInstances = describeResult.GetInstances();
            unsigned int n_instances = awsInstances.size();
            awsc_ops_instance *instances = (awsc_ops_instance *)ma_alloc(state->mem->arena_client, sizeof(awsc_ops_instance) * n_instances);
            int i = 0;
            for (auto awsInstance: awsInstances) {
                awsc_ops_instance *instance = instances + i++;

                const Aws::String& awsInstanceId = awsInstance.GetInstanceId();
                instance->id = (char *)ma_alloc(state->mem->arena_client, awsInstanceId.size() + 1);
                strcpy(instance->id, awsInstanceId.c_str());

                const Aws::String& awsAmiHostname = awsInstance.GetHostname();
                instance->name = (char *)ma_alloc(state->mem->arena_client, awsAmiHostname.size() + 1);
                strcpy(instance->name, awsAmiHostname.c_str());

                const Aws::String& awsAmiStatus = awsInstance.GetStatus();
                instance->status = (char *)ma_alloc(state->mem->arena_client, awsAmiStatus.size() + 1);
                strcpy(instance->status, awsAmiStatus.c_str());
            }
            result.instances = instances;
            result.n_instances = i;
        }
    }
snapshot_restore:
    ma_snapshot_restore(snapshot);

    return result;
}

char *get_exp_vpc_desc(bool is_prod) {
    return is_prod ? (char *)"Prod" : (char *)"NonProd";
}

static maybe_string lookup_vpcid(Aws::EC2::EC2Client *ec2Client, char *vpc_desc, ma_ctx *arena) {
    maybe_string result = {};
    Aws::EC2::Model::DescribeVpcsRequest descReq;
    auto descOutcome = ec2Client->DescribeVpcs(descReq);
    if (!descOutcome.IsSuccess()) {
        auto error = descOutcome.GetError();
        const Aws::String& message = error.GetMessage();
        result.error = (char *)ma_alloc(arena, message.size() + 1);
        strcpy(result.error, message.c_str());
    } else {
        const Aws::EC2::Model::DescribeVpcsResponse& descResp = descOutcome.GetResult();
        const Aws::Vector<Aws::EC2::Model::Vpc>& awsVpcs = descResp.GetVpcs();
        bool found = false;
        for (Aws::EC2::Model::Vpc awsVpc : awsVpcs) {
            const Aws::Vector<Aws::EC2::Model::Tag>& awsVpcTags = awsVpc.GetTags();
            for (Aws::EC2::Model::Tag awsVpcTag : awsVpcTags) {
                const Aws::String& tagKey = awsVpcTag.GetKey();
                if (strcmp(tagKey.c_str(), "Name") == 0) {
                    const Aws::String& tagVal = awsVpcTag.GetValue();
                    if (strcmp(tagVal.c_str(), vpc_desc) == 0) {
                        found = true;
                        const Aws::String& awsVpcId = awsVpc.GetVpcId();
                        result.value = (char *)ma_alloc(arena, awsVpcId.size() + 1);
                        strcpy(result.value, awsVpcId.c_str());
                        break;
                    }
                }
            }
            if (found)
                break;
        }
        if (!found)
            result.error = "Could not find vpc with description";
    }
    return result;
}

maybe_string lookup_subnetid(Aws::EC2::EC2Client *ec2Client, char *vpcid, char *availability_zone, ma_ctx *arena) {
    maybe_string result = {};
    Aws::EC2::Model::DescribeSubnetsRequest descReq;

    Aws::EC2::Model::Filter filterVpcId;
    filterVpcId.SetName("vpc-id");
    Aws::Vector<Aws::String> filterVpcValues;
    filterVpcValues.push_back(vpcid);
    filterVpcId.SetValues(filterVpcValues);

    Aws::EC2::Model::Filter filterAvailabilityZone;
    filterAvailabilityZone.SetName("availability-zone");
    Aws::Vector<Aws::String> filterAvailabilityZoneValues;
    filterAvailabilityZoneValues.push_back(availability_zone);
    filterAvailabilityZone.SetValues(filterAvailabilityZoneValues);

    Aws::Vector<Aws::EC2::Model::Filter> filters;
    filters.push_back(filterVpcId);
    filters.push_back(filterAvailabilityZone);

    descReq.SetFilters(filters);

    auto descOutcome = ec2Client->DescribeSubnets(descReq);
    if (!descOutcome.IsSuccess()) {
        auto error = descOutcome.GetError();
        const Aws::String& message = error.GetMessage();
        result.error = (char *)ma_alloc(arena, message.size() + 1);
        strcpy(result.error, message.c_str());
    } else {
        const Aws::EC2::Model::DescribeSubnetsResponse& descRes = descOutcome.GetResult();
        const Aws::Vector<Aws::EC2::Model::Subnet> awsSubnets = descRes.GetSubnets();
        for (auto awsSubnet : awsSubnets) {
            const Aws::String& awsZone = awsSubnet.GetAvailabilityZone();
            if (strcmp(awsZone.c_str(), availability_zone) == 0) {
                const Aws::String& awsSubnetId = awsSubnet.GetSubnetId();
                result.value = (char *)ma_alloc(arena, awsSubnetId.size() + 1);
                strcpy(result.value, awsSubnetId.c_str());
                break;
            }
        }
        if (!result.value) {
            result.error = "Could not find subnet id";
        }
    }
    return result;
}

awsc_ops_create_result awsc_ops_create(awsc_client *ops_client, char *layer_name, char *stack_name, char *env, awsc_ops_create_options *options) {
    awsc_ops_create_result result = {};
    awsc_state *state = ops_client->state;
    Aws::EC2::EC2Client *ec2Client = NULL;

    if (options->availability_zone) {
        if (options->ec2_client) {
            ec2Client = (Aws::EC2::EC2Client *)options->ec2_client->aws_client_handle;
        } else {
            result.error = "Need ec2 client when availability zone is specified";
            return result;
        }
    }

    ma_linear_snapshot *snapshot = ma_snapshot_save(state->mem->arena_aws);

    Aws::OpsWorks::OpsWorksClient *opsClient = (Aws::OpsWorks::OpsWorksClient *)ops_client->aws_client_handle;

    maybe_stack_and_layer_ids stack_and_layer_ids = lookup_stack_and_layer_ids(opsClient, stack_name, layer_name, state->mem->arena_client);
    if (stack_and_layer_ids.error) {
        result.error = stack_and_layer_ids.error;
        goto snapshot_restore;
    } else {
        char *stack_id = stack_and_layer_ids.stack_id;
        char *layer_id = stack_and_layer_ids.layer_id;
        Aws::OpsWorks::Model::CreateInstanceRequest createReq;

        char *instance_type = (char *)(options->instance_type ? options->instance_type : "t2.medium");
        createReq.SetInstanceType(instance_type);

        char *root_device_type = NULL;
        if (options->root_device) {
            root_device_type = options->root_device;
        } else {
            char *instance_store_families[] = {
                "m3.", "c3.", "r3.", "g2.", "i2.", "d2."
            };
            for (unsigned int family_idx = 0; family_idx < sizeof(instance_store_families) / sizeof(char *); family_idx++) {
                char *family_prefix = instance_store_families[family_idx];
                if (strncmp(family_prefix, instance_type, 3) == 0) {
                    root_device_type = "instance-store";
                    break;
                }
            }
            if (!root_device_type) {
                root_device_type = "ebs";
            }
        }
        if (strcmp(root_device_type, "ebs") == 0) {
            createReq.SetRootDeviceType(Aws::OpsWorks::Model::RootDeviceType::ebs);
        } else if (strcmp(root_device_type, "instance-store") == 0) {
            createReq.SetRootDeviceType(Aws::OpsWorks::Model::RootDeviceType::instance_store);
        } else {
            result.error = "Unknown root device type";
            goto snapshot_restore;
        }

        if (options->availability_zone) {
            assert(ec2Client);
            bool is_prod = (strcmp(env, "prod") == 0);
            char *vpc_desc = get_exp_vpc_desc(is_prod);
            maybe_string vpcid = lookup_vpcid(ec2Client, vpc_desc, state->mem->arena_client);
            if (vpcid.error) {
                result.error = vpcid.error;
                goto snapshot_restore;
            }
            maybe_string subnetid = lookup_subnetid(ec2Client, vpcid.value, options->availability_zone, state->mem->arena_client);
            if (subnetid.error) {
                result.error = subnetid.error;
                goto snapshot_restore;
            }

            createReq.SetSubnetId(subnetid.value);
        }
        // TODO support creating multiple instances
        // this should make creating multiples much faster
        // can just iterate through here
        Aws::OpsWorks::Model::GetHostnameSuggestionRequest hostReq;
        hostReq.SetLayerId(layer_id);
        auto hostOutcome = opsClient->GetHostnameSuggestion(hostReq);
        if (!hostOutcome.IsSuccess()) {
            auto error = hostOutcome.GetError();
            const Aws::String& message = error.GetMessage();
            result.error = (char *)ma_alloc(state->mem->arena_client, message.size() + 1);
            strcpy(result.error, message.c_str());
            goto snapshot_restore;
        }
        const Aws::OpsWorks::Model::GetHostnameSuggestionResult& hostRes = hostOutcome.GetResult();
        const Aws::String& hostStr = hostRes.GetHostname();
        createReq.SetHostname(hostStr);
        createReq.SetStackId(stack_id);

        Aws::Vector<Aws::String> awsLayers;
        awsLayers.push_back(layer_id);
        if (options->add_base_layer) {
            // TODO look up base layer here
            // and add it to the layers list
        }
        createReq.SetLayerIds(awsLayers);

        // TODO root device type is always necessary
        // consolidate with above
        createReq.SetRootDeviceType(Aws::OpsWorks::Model::RootDeviceType::ebs);

        auto createOutcome = opsClient->CreateInstance(createReq);
        if (!createOutcome.IsSuccess()) {
            auto error = createOutcome.GetError();
            const Aws::String& message = error.GetMessage();
            result.error = (char *)ma_alloc(state->mem->arena_client, message.size() + 1);
            strcpy(result.error, message.c_str());
            goto snapshot_restore;
        }
        const Aws::OpsWorks::Model::CreateInstanceResult& createRes = createOutcome.GetResult();
        const Aws::String& instanceIdStr = createRes.GetInstanceId();
        result.id = (char *)ma_alloc(state->mem->arena_client, instanceIdStr.size() + 1);
        strcpy(result.id, instanceIdStr.c_str());
    }

snapshot_restore:
    ma_snapshot_restore(snapshot);

    return result;
}
