#include <stdio.h>
#include <stdint.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <pthread.h>

#include "netlink.h"
#include "configuration.h"
#include "query.h"
#include "query_queue.h"
#include "plugins.h"


policy_context_t context;

static void* plugin_thread_init(void* arg);
static void* decider_thread_init(void* arg);
static int async_callback(int plugin_id, int query_id, int result);


typedef struct { unsigned char b[3]; } be24, le24;


int poll_schemes(uint64_t stptr, char* hostname, unsigned char* cert_data, size_t len) {
	int i;
	query_t* query;
	/* Validation */
	query = create_query(context.plugin_count, stptr, hostname, cert_data, len);
	enqueue(context.decider_queue, query);
	for (i = 0; i < context.plugin_count; i++) {
		printf("Enqueuing query\n");
		enqueue(context.plugins[i].queue, query);
		printf("query enqueued\n");
	}
	return 0;
}


int main() {
	int i;
	pthread_t decider_thread;
	pthread_t* plugin_threads;
	thread_param_t decider_thread_params;
	thread_param_t* plugin_thread_params;

	load_config(&context);
	init_addons(context.addons, context.addon_count, context.plugin_count);
	init_plugins(context.addons, context.addon_count, context.plugins, context.plugin_count);
	print_addons(context.addons, context.addon_count);
	print_plugins(context.plugins, context.plugin_count);

	/* Decider thread (runs CA system and aggregates plugin verdicts */
	decider_thread_params.plugin_id = -1;
	context.decider_queue = make_queue("decider");
	pthread_create(&decider_thread, NULL, decider_thread_init, &decider_thread_params);


	/* Plugin Threading */
	plugin_thread_params = (thread_param_t*)malloc(sizeof(thread_param_t) * context.plugin_count);
	plugin_threads = (pthread_t*)malloc(sizeof(pthread_t) * context.plugin_count);
	for (i = 0; i < context.plugin_count; i++) {
		context.plugins[i].queue = make_queue(context.plugins[i].name); // XXX relocate this
		plugin_thread_params[i].plugin_id = i;
		pthread_create(&plugin_threads[i], NULL, plugin_thread_init, &plugin_thread_params[i]);
	}

	listen_for_queries();

	// Cleanup
	for (i = 0; i < context.plugin_count; i++) {
		free_queue(context.plugins[i].queue); // XXX relocate this
		pthread_join(plugin_threads[i], NULL);
	}
	pthread_join(decider_thread, NULL);
	free_queue(context.decider_queue);
	close_plugins(context.plugins, context.plugin_count);
	close_addons(context.addons, context.addon_count);
	free(plugin_thread_params);
	free(plugin_threads);
	return 0;
}

void* plugin_thread_init(void* arg) {
	queue_t* queue;
	int plugin_id;
	thread_param_t* params;
	plugin_t* plugin;
	query_t* query;
	int result;

	params = (thread_param_t*)arg;
	plugin_id = params->plugin_id;
	plugin = &context.plugins[plugin_id];
	queue = plugin->queue;
	
	if (plugin->generic_init_func != NULL) {
		if (plugin->type == PLUGIN_TYPE_SYNCHRONOUS) {
			plugin->init_sync(plugin_id);
		}
		else {
			plugin->init_async(plugin_id, async_callback);
		}
	}
	while (1) {
		printf("Dequeuing query\n");
		query = dequeue(queue);
		printf("Query dequeued\n");
		if (plugin->type == PLUGIN_TYPE_SYNCHRONOUS) {
			result = query_sync_plugin(plugin, plugin_id, query);
			query->responses[plugin_id] = result;
			pthread_mutex_lock(&query->mutex);
			query->num_responses++;
			printf("%d plugins have submitted an answer\n", query->num_responses);
			if (query->num_responses == context.plugin_count) {
				pthread_cond_signal(&query->threshold_met);
			}
			pthread_mutex_unlock(&query->mutex);
		}
		else if (plugin->type == PLUGIN_TYPE_ASYNCHRONOUS) {
			query_async_plugin(plugin, plugin_id, query);
		}
	}
	return NULL;
}

void* decider_thread_init(void* arg) {
	queue_t* queue;
	query_t* query;
	int ca_system_response;
	queue = context.decider_queue;
	// XXX Init CA system here
	while (1) {
		query = dequeue(queue);
		// XXX actually query CA system here
		ca_system_response = PLUGIN_RESPONSE_ABSTAIN;
		pthread_mutex_lock(&query->mutex);
		while (query->num_responses < context.plugin_count) {
			pthread_cond_wait(&query->threshold_met, &query->mutex);
		}
		pthread_mutex_unlock(&query->mutex);
		printf("All plugins have submitted an answer\n");
		free_query(query);
		send_response(query->state_pointer, 1);
	}
	return NULL;
}

int async_callback(int plugin_id, int query_id, int result) {
	query_t* query;

	query = (query_t*)(query_id);
	if (query == NULL) {
		return 0; /* let plugin know this result timed out */
	}

	query->responses[plugin_id] = result;
	pthread_mutex_lock(&query->mutex);
	query->num_responses++;
	if (query->num_responses == context.plugin_count) {
	printf("%d plugins have submitted an answer\n", query->num_responses);
		pthread_cond_signal(&query->threshold_met);
	}
	pthread_mutex_unlock(&query->mutex);
	printf("Asynchronous callback invoked by plugin %d!\n", plugin_id);
	return 1; /* let plugin know the callback was successful */
}
