#include <memarena.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "awsc.h"

int main() {
    void *memory = calloc(1, 1024 * 1000 * 10);

    size_t aws = 1024 * 1000;
    size_t curl = 1024 * 5000;
    size_t client = 1024 * 3000;
    awsc_state *state = awsc_state_create(memory, aws, curl, client);
    awsc_state_init(state);

    awsc_creds *creds = NULL;
    awsc_config *config = awsc_create_config(state, AWSC_REGION_US_EAST, creds);

    awsc_client *sqs_client = awsc_client_create_sqs(state, config);

    awsc_sqs_url_result url_result = awsc_sqs_url(sqs_client, "mapzen-tiles-dev-us-east");
    assert(!url_result.error);

    awsc_sqs_n_result n_result = awsc_sqs_n(sqs_client, url_result.url);
    assert(!n_result.error);

    printf("%zu\n", n_result.n);

    awsc_state_destroy(state);
    free(memory);

    printf("done\n");
}
