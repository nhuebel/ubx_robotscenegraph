#include <zyre.h>
#include <jansson.h>
#include <uuid/uuid.h>
#include <string.h>
#include "swmzyre.h"

void query_destroy (query_t **self_p) {
        assert (self_p);
        if(*self_p) {
            query_t *self = *self_p;
            destroy_message(self->msg);
            free (self);
            *self_p = NULL;
        }
}

void destroy_message(json_msg_t *msg) {
	free(msg->metamodel);
	free(msg->model);
	free(msg->type);
//	zstr_free(msg->payload);
	free(msg->payload);
	free(msg);
}

void destroy_component (component_t **self_p) {
    assert (self_p);
    if(*self_p) {
    	component_t *self = *self_p;
    	zactor_destroy (&self->communication_actor);
    	zclock_sleep (100);
    	zyre_stop (self->local);
		printf ("[%s] Stopping zyre node.\n", self->name);
		zclock_sleep (100);
		zyre_destroy (&self->local);
		printf ("[%s] Destroying component.\n", self->name);
        json_decref(self->config);
        //free memory of all items from the query list
        query_t *it;
        while(zlist_size (self->query_list) > 0){
        	it = zlist_pop (self->query_list);
        	query_destroy(&it);
        }
        zlist_destroy (&self->query_list);

        free (self);
        *self_p = NULL;
    }
}

query_t * query_new (const char *uid, const char *requester, json_msg_t *msg, zactor_t *loop) {
        query_t *self = (query_t *) zmalloc (sizeof (query_t));
        if (!self)
            return NULL;
        self->uid = uid;
        self->requester = requester;
        self->msg = msg;
        self->loop = loop;

        return self;
}

static void communication_actor (zsock_t *pipe, void *args)
{
	component_t *self = (component_t*) args;
	zpoller_t *poller =  zpoller_new (zyre_socket(self->local), pipe , NULL);
	zsock_signal (pipe, 0);

	while((!zsys_interrupted)&&(self->alive == 1)){
		//printf("[%s] Queries in queue: %d \n",self->name,zlist_size (self->query_list));
		void *which = zpoller_wait (poller, ZMQ_POLL_MSEC);
		if (which == zyre_socket(self->local)) {
			zmsg_t *msg = zmsg_recv (which );
			if (!msg) {
				printf("[%s] interrupted!\n", self->name);
			}
			//zmsg_print(msg); printf("msg end\n");
			char *event = zmsg_popstr (msg);
			if (streq (event, "ENTER")) {
				handle_enter (self, msg);
			} else if (streq (event, "EXIT")) {
				handle_exit (self, msg);
			} else if (streq (event, "SHOUT")) {
				char *rep;
				handle_shout (self, msg, &rep);
				if (rep) {
					printf("communication actor received reply: %s\n",rep);
					zstr_sendf(pipe,"%s",rep);
					printf("sent");
					zstr_free(&rep);
				}
			} else if (streq (event, "WHISPER")) {
				handle_whisper (self, msg);
			} else if (streq (event, "JOIN")) {
				handle_join (self, msg);
			} else if (streq (event, "EVASIVE")) {
				handle_evasive (self, msg);
			} else {
				zmsg_print(msg);
			}
			zstr_free (&event);
			zmsg_destroy (&msg);
		}
		else if (which == pipe) {
			zmsg_t *msg = zmsg_recv (which);
			if (!msg)
				break; //  Interrupted
			char *command = zmsg_popstr (msg);
			if (streq (command, "$TERM"))
				self->alive = 0;

			}
	}
	zpoller_destroy (&poller);
}

component_t* new_component(json_t *config) {
	component_t *self = (component_t *) zmalloc (sizeof (component_t));
    if (!self)
        return NULL;
    if (!config)
            return NULL;

    self->config = config;

	self->name = json_string_value(json_object_get(config, "short-name"));
    if (!self->name) {
        destroy_component (&self);
        return NULL;
    }
	self->timeout = json_integer_value(json_object_get(config, "timeout"));
    if (self->timeout <= 0) {
    	destroy_component (&self);
        return NULL;
    }

	self->no_of_updates = json_integer_value(json_object_get(config, "no_of_updates"));
    if (self->no_of_updates <= 0) {
    	destroy_component (&self);
        return NULL;
    }

	self->no_of_queries = json_integer_value(json_object_get(config, "no_of_queries"));
    if (self->no_of_queries <= 0) {
    	destroy_component (&self);
        return NULL;
    }

	self->no_of_fcn_block_calls = json_integer_value(json_object_get(config, "no_of_fcn_block_calls"));
    if (self->no_of_fcn_block_calls <= 0) {
    	destroy_component (&self);
        return NULL;
    }
	//  Create local gossip node
	self->local = zyre_new (self->name);
    if (!self->local) {
    	destroy_component (&self);
        return NULL;
    }
	printf("[%s] my local UUID: %s\n", self->name, zyre_uuid(self->local));

	/* config is a JSON object */
	// set values for config file as zyre header.
	const char *key;
	json_t *value;
	json_object_foreach(config, key, value) {
		const char *header_value;
		if(json_is_string(value)) {
			header_value = json_string_value(value);
		} else {
			header_value = json_dumps(value, JSON_ENCODE_ANY);
		}
		printf("header key value pair\n");
		printf("%s %s\n",key,header_value);
		zyre_set_header(self->local, key, "%s", header_value);
	}

	//create a list to store queries...
	self->query_list = zlist_new();
	if ((!self->query_list)&&(zlist_size (self->query_list) == 0)) {
		destroy_component (&self);
		return NULL;
	}

	self->alive = 1; //will be used to quit program after answer to query is received

	int rc;
	if(!json_is_null(json_object_get(config, "gossip_endpoint"))) {
		//  Set up gossip network for this node
		zyre_gossip_connect (self->local, "%s", json_string_value(json_object_get(config, "gossip_endpoint")));
		printf("[%s] using gossip with gossip hub '%s' \n", self->name,json_string_value(json_object_get(config, "gossip_endpoint")));
	} else {
		printf("[%s] WARNING: no local gossip communication is set! \n", self->name);
	}
	rc = zyre_start (self->local);
	assert (rc == 0);
	///TODO: romove hardcoding of group name!
	self->localgroup = strdup("local");
	zyre_join (self->local, self->localgroup);
	//  Give time for them to connect
	zclock_sleep (1000);

	self->communication_actor = zactor_new (communication_actor, self);
	assert (self->communication_actor);
	///TODO: move to debug
	zstr_sendx (self->communication_actor, "VERBOSE", NULL);

	return self;
}

json_t * load_config_file(char* file) {
    json_error_t error;
    json_t * root;
    root = json_load_file(file, JSON_ENSURE_ASCII, &error);
	if(!root) {
		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
    	return NULL;
    }
    printf("[%s] config file: %s\n", json_string_value(json_object_get(root, "short-name")), json_dumps(root, JSON_ENCODE_ANY));

    return root;
}



int decode_json(char* message, json_msg_t *result) {
	/**
	 * decodes a received msg to json_msg types
	 *
	 * @param received msg as char*
	 * @param json_msg_t* at which the result is stored
	 *
	 * @return returns 0 if successful and -1 if an error occurred
	 */
    json_t *root;
    json_error_t error;
    root = json_loads(message, 0, &error);
    int ret = 0;

    if(!root) {
    	printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
    	return -1;
    }

    if (json_object_get(root, "metamodel")) {
    	result->metamodel = strdup(json_string_value(json_object_get(root, "metamodel")));
    } else {
    	printf("Error parsing JSON string! Does not conform to msg model.\n");
    	ret = -1;
    	goto cleanup;
    }
    if (json_object_get(root, "model")) {
		result->model = strdup(json_string_value(json_object_get(root, "model")));
	} else {
		printf("Error parsing JSON string! Does not conform to msg model.\n");
		ret = -1;
		goto cleanup;
	}
    if (json_object_get(root, "type")) {
		result->type = strdup(json_string_value(json_object_get(root, "type")));
	} else {
		printf("Error parsing JSON string! Does not conform to msg model.\n");
		ret = -1;
		goto cleanup;
	}
    if (json_object_get(root, "payload")) {
    	result->payload = strdup(json_dumps(json_object_get(root, "payload"), JSON_ENCODE_ANY));
	} else {
		printf("Error parsing JSON string! Does not conform to msg model.\n");
		ret = -1;
		goto cleanup;
	}
cleanup:
    json_decref(root);
    return ret;
}

char* encode_json_message_from_file(component_t* self, char* message_file) {
    json_error_t error;
    json_t * pl;
    // create the payload, i.e., the query
    pl = json_load_file(message_file, JSON_ENSURE_ASCII, &error);
    printf("[%s] message file: %s\n", message_file, json_dumps(pl, JSON_ENCODE_ANY));
    if(!pl) {
    	printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
    	return NULL;
    }

    return encode_json_message(self, pl);
}

char* encode_json_message_from_string(component_t* self, char* message) {
    json_t *pl;
    json_error_t error;
    pl = json_loads(message, 0, &error);

    printf("[%s] message : %s\n", self->name , json_dumps(pl, JSON_ENCODE_ANY));
	if(!pl) {
		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
		return NULL;
	}

    return encode_json_message(self, pl);
}

char* encode_json_message(component_t* self, json_t* message) {
    json_t * pl = message;
    // create the payload, i.e., the query


    if(!pl) {
    	printf("Error! No JSON message was passed to encode_json:message\n");
    	return NULL;
    }

    // extract queryId
	if(json_object_get(pl,"queryId") == 0) { // no queryID in message, so we skip it here
		printf("[%s] send_json_message: No queryId found, adding one.\n", self->name);
		zuuid_t *uuid = zuuid_new ();
		assert(uuid);
		const char* uuid_as_string = zuuid_str_canonical(uuid);
	    json_object_set_new(pl, "queryId", json_string(uuid_as_string));
	    free(uuid);
	}

	const char* query_id = json_string_value(json_object_get(pl,"queryId"));
	//printf("[%s] send_json_message: query_id = %s:\n", self->name, query_id);

	// pack it into the standard msg envelope
	json_t *env;
    env = json_object();
	json_object_set_new(env, "metamodel", json_string("SHERPA"));
	json_object_set_new(env, "model", json_string("RSGQuery"));
	json_object_set_new(env, "type", json_string("RSGQuery"));
	json_object_set(env, "payload", pl);

	// add it to the query list
	json_msg_t *msg = (json_msg_t *) zmalloc (sizeof (json_msg_t));
	msg->metamodel = strdup("SHERPA");
	msg->model = strdup("RSGQuery");
	msg->type = strdup("RSGQuery");
	msg->payload = json_dumps(pl, JSON_ENCODE_ANY);
	query_t * q = query_new(query_id, zyre_uuid(self->local), msg, NULL);
	zlist_append(self->query_list, q);

    char* ret = json_dumps(env, JSON_ENCODE_ANY);
	printf("[%s] send_json_message: message = %s:\n", self->name, ret);

	json_decref(env);

    return ret;
}

int shout_message(component_t* self, char* message) {
	 return zyre_shouts(self->local, self->localgroup, "%s", message);
}

char* wait_for_reply(component_t* self, char *msg, int timeout) {

	char* ret = NULL;
	if (timeout <= 0) {
		printf("[%s] Timeout has to be >0!\n",self->name);
		return ret;
	}

    // timestamp for timeout
    struct timespec ts = {0,0};
    struct timespec curr_time = {0,0};
    if (clock_gettime(CLOCK_MONOTONIC,&ts)) {
		printf("[%s] Could not assign time stamp!\n",self->name);
		return ret;
	}
    if (clock_gettime(CLOCK_MONOTONIC,&curr_time)) {
		printf("[%s] Could not assign time stamp!\n",self->name);
		return ret;
	}

    json_error_t error;
    json_t *sent_msg;
    sent_msg = json_loads(msg, 0, &error);
    if (!sent_msg){
    	printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
    	printf("[%s] Message passed to wait_for_reply is no valid JSON.\n", self->name);
    	return ret;
    }
    // because of implementation inconsistencies between SWM and CM, we have to check for UID and queryId
    char *queryID = json_string_value(json_object_get(json_object_get(sent_msg,"payload"),"queryId"));
    if(!queryID) {
    	queryID = json_string_value(json_object_get(json_object_get(sent_msg,"payload"),"UID"));
    	if(!queryID) {
        	printf("[%s] Message has no queryID to wait for: %s\n", self->name, msg);
        	goto cleanup;
    	}
    }

    //do this with poller?
    zsock_set_rcvtimeo (self->communication_actor, timeout); //set timeout for socket
    while (!zsys_interrupted){
    	ret = zstr_recv (self->communication_actor);
		if (ret){ //if ret is query we were waiting for in this thread
			//printf("[%s] wait_for_reply received answer %s:\n", self->name, ret);
			json_t *pl;
			pl = json_loads(ret, 0, &error);
			if(!pl) {
				printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
				goto cleanup;
			}
			//streq cannot take NULL, so check before
			char *received_queryID = json_string_value(json_object_get(json_object_get(pl,"payload"),"queryId"));
			if(!received_queryID) {
				received_queryID = json_string_value(json_object_get(json_object_get(pl,"payload"),"UID"));
				if(!received_queryID) {
					printf("[%s] Received message has no queryID: %s\n", self->name, msg);
					goto cleanup;
				}
			}
			if (streq(received_queryID,queryID)){
				printf("[%s] wait_for_reply received answer to query %s:\n", self->name, ret);
				json_decref(pl);
				break;
			}
			json_decref(pl);

		}
    	//if it is not the query we were waiting for in this thread, go back to recv if timeout has not happened yet
		if (!clock_gettime(CLOCK_MONOTONIC,&curr_time)) {
			// if timeout, stop component
			double curr_time_msec = curr_time.tv_sec*1.0e3 +curr_time.tv_nsec*1.0e-6;
			double ts_msec = ts.tv_sec*1.0e3 +ts.tv_nsec*1.0e-6;
			if (curr_time_msec - ts_msec > timeout) {
				printf("[%s] Timeout! No query answer received.\n",self->name);
				destroy_component(&self);
				break;
			}
		} else {
			printf ("[%s] could not get current time\n", self->name);
		}
    }

cleanup:
    json_decref(sent_msg);
    return ret;
}


char* send_query(component_t* self, char* query_type, json_t* query_params) {
	/**
	 * creates a query msg for the world model and adds it to the query list
	 *
	 * @param string query_type as string containing one of the available query types ["GET_NODES", "GET_NODE_ATTRIBUTES", "GET_NODE_PARENTS", "GET_GROUP_CHILDREN", "GET_ROOT_NODE", "GET_REMOTE_ROOT_NODES", "GET_TRANSFORM", "GET_GEOMETRY", "GET_CONNECTION_SOURCE_IDS", "GET_CONNECTION_TARGET_IDS"]
	 * @param json_t query_params json object containing all information required by the query; check the rsg-query-schema.json for details
	 *
	 * @return the string encoded JSON msg that can be sent directly via zyre. Must be freed by user! Returns NULL if wrong json types are passed in.
	 */

	// create the payload, i.e., the query
    json_t *pl;
    pl = json_object();
    json_object_set(pl, "@worldmodeltype", json_string("RSGQuery"));
    json_object_set(pl, "query", json_string(query_type));
	zuuid_t *uuid = zuuid_new ();
	assert(uuid);
    json_object_set(pl, "queryId", json_string(zuuid_str_canonical(uuid)));
	
	if (json_object_size(query_params)>0) {
		const char *key;
		json_t *value;
		json_object_foreach(query_params, key, value) {
			json_object_set(pl, key, value);
		}
	}

	// pack it into the standard msg envelope
	json_t *env;
    env = json_object();
	json_object_set(env, "metamodel", json_string("SHERPA"));
	json_object_set(env, "model", json_string("RSGQuery"));
	json_object_set(env, "type", json_string("RSGQuery"));
	json_object_set(env, "payload", pl);
	
	// add it to the query list
	json_msg_t *msg = (json_msg_t *) zmalloc (sizeof (json_msg_t));
	msg->metamodel = strdup("SHERPA");
	msg->model = strdup("RSGQuery");
	msg->type = strdup("RSGQuery");
	msg->payload = strdup(json_dumps(pl, JSON_ENCODE_ANY));
	query_t * q = query_new(zuuid_str_canonical(uuid), zyre_uuid(self->local), msg, NULL);
	zlist_append(self->query_list, q);

    char* ret = json_dumps(env, JSON_ENCODE_ANY);
	
	json_decref(env);
    json_decref(pl);
    return ret;
}

char* send_update(component_t* self, char* operation, json_t* update_params) {
	/**
	 * creates an update msg for the world model and adds it to the query list
	 *
	 * @param string operation as string containing one of the available operations ["CREATE","CREATE_REMOTE_ROOT_NODE","CREATE_PARENT","UPDATE_ATTRIBUTES","UPDATE_TRANSFORM","UPDATE_START","UPDATE_END","DELETE_NODE","DELETE_PARENT"]
	 * @param json_t update_params json object containing all information required by the update; check the rsg-update-schema.json for details
	 *
	 * @return the string encoded JSON msg that can be sent directly via zyre. Must be freed by user! Returns NULL if wrong json types are passed in.
	 */

	// create the payload, i.e., the query
    json_t *pl;
    pl = json_object();
    json_object_set(pl, "@worldmodeltype", json_string("RSGUpdate"));
    json_object_set(pl, "operation", json_string(operation));
    json_object_set(pl, "node", json_string(operation));
	zuuid_t *uuid = zuuid_new ();
	assert(uuid);
    json_object_set(pl, "queryId", json_string(zuuid_str_canonical(uuid)));

    if (!json_object_get(update_params,"node")) {
    	printf("[%s:send_update] No node object on parameters",self->name);
    	return NULL;
    }
	if (json_object_size(update_params)>0) {
		const char *key;
		json_t *value;
		json_object_foreach(update_params, key, value) {
			json_object_set(pl, key, value);
		}
	}
	// pack it into the standard msg envelope
	json_t *env;
    env = json_object();
	json_object_set(env, "metamodel", json_string("SHERPA"));
	json_object_set(env, "model", json_string("RSGQuery"));
	json_object_set(env, "type", json_string("RSGQuery"));
	json_object_set(env, "payload", pl);

	// add it to the query list
	json_msg_t *msg = (json_msg_t *) zmalloc (sizeof (json_msg_t));
	msg->metamodel = strdup("SHERPA");
	msg->model = strdup("RSGQuery");
	msg->type = strdup("RSGQuery");
	msg->payload = strdup(json_dumps(pl, JSON_ENCODE_ANY));
	query_t * q = query_new(zuuid_str_canonical(uuid), zyre_uuid(self->local), msg, NULL);
	zlist_append(self->query_list, q);

    char* ret = json_dumps(env, JSON_ENCODE_ANY);

	json_decref(env);
    json_decref(pl);
    return ret;
}

void handle_enter(component_t *self, zmsg_t *msg) {
	assert (zmsg_size(msg) == 4);
	char *peerid = zmsg_popstr (msg);
	char *name = zmsg_popstr (msg);
	zframe_t *headers_packed = zmsg_pop (msg);
	assert (headers_packed);
	zhash_t *headers = zhash_unpack (headers_packed);
	assert (headers);
	printf("header type %s\n",(char *) zhash_lookup (headers, "type"));
	char *address = zmsg_popstr (msg);
	printf ("[%s] ENTER %s %s <headers> %s\n", self->name, peerid, name, address);
	zstr_free(&peerid);
	zstr_free(&name);
	zframe_destroy(&headers_packed);
	zhash_destroy(&headers);
	zstr_free(&address);
}

void handle_exit(component_t *self, zmsg_t *msg) {
	assert (zmsg_size(msg) == 2);
	char *peerid = zmsg_popstr (msg);
	char *name = zmsg_popstr (msg);
	printf ("[%s] EXIT %s %s\n", self->name, peerid, name);
	zstr_free(&peerid);
	zstr_free(&name);
}

void handle_whisper (component_t *self, zmsg_t *msg) {
	assert (zmsg_size(msg) == 3);
	char *peerid = zmsg_popstr (msg);
	char *name = zmsg_popstr (msg);
	char *message = zmsg_popstr (msg);
	printf ("[%s] WHISPER %s %s %s\n", self->name, peerid, name, message);
	zstr_free(&peerid);
	zstr_free(&name);
	zstr_free(&message);
}

void handle_shout(component_t *self, zmsg_t *msg, char **rep) {
	assert (zmsg_size(msg) == 4);
	*rep = NULL;
	char *peerid = zmsg_popstr (msg);
	char *name = zmsg_popstr (msg);
	char *group = zmsg_popstr (msg);
	char *message = zmsg_popstr (msg);
	printf ("[%s] SHOUT %s %s %s %s\n", self->name, peerid, name, group, message);
	json_msg_t *result = (json_msg_t *) zmalloc (sizeof (json_msg_t));
	if (decode_json(message, result) == 0) {
//		printf ("[%s] message type %s\n", self->name, result->type);
		if (streq (result->type, "RSGUpdateResult")) {
			// load the payload as json
			json_t *payload;
			json_error_t error;
			payload= json_loads(result->payload,0,&error);
			if(!payload) {
				printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
			} else {
				query_t *it = zlist_first(self->query_list);
				while (it != NULL) {
					if(json_object_get(payload,"queryId") == 0) { // no queryIt in message, so we skip it here
						printf("Skipping RSGUpdateResult message without queryId");
						break;
					}
					if (streq(it->uid,json_string_value(json_object_get(payload,"queryId")))) {
						printf("[%s] received answer to query %s:\n %s\n ", self->name,it->uid,result->payload);
						*rep = strdup(result->payload);
//						free(it->msg->payload);
						query_t *dummy = it;
						it = zlist_next(self->query_list);
						zlist_remove(self->query_list,dummy);
						query_destroy(&dummy);
					    json_decref(payload);
						break;
					}
				}
			}
		} else if (streq (result->type, "RSGQueryResult")) {
			// load the payload as json
			json_t *payload;
			json_error_t error;
			payload= json_loads(result->payload,0,&error);
			if(!payload) {
				printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
			} else {
				query_t *it = zlist_first(self->query_list);
				while (it != NULL) {
					if(json_object_get(payload,"queryId") == 0) { // no queryIt in message, so we skip it here
						printf("Skipping RSGQueryResult message without queryId\n");
						break;
					}
					if (streq(it->uid,json_string_value(json_object_get(payload,"queryId")))) {
						printf("[%s] received answer to query %s of type %s:\n Query:\n %s\n Result:\n %s \n", self->name,it->uid,result->type,it->msg->payload, result->payload);
						*rep = strdup(result->payload);
						query_t *dummy = it;
						it = zlist_next(self->query_list);
						zlist_remove(self->query_list,dummy);
						query_destroy(&dummy);
					    json_decref(payload);
						break;
					}
				}
			}
		} else if (streq (result->type, "RSGFunctionBlockResult")) {
			// load the payload as json
			json_t *payload;
			json_error_t error;
			payload= json_loads(result->payload,0,&error);
			if(!payload) {
				printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
			} else {
				query_t *it = zlist_first(self->query_list);
				while (it != NULL) {
					if(json_object_get(payload,"queryId") == 0) { // no queryIt in message, so we skip it here
						printf("Skipping RSGFunctionBlockResult message without queryId\n");
						break;
					}
					if (streq(it->uid,json_string_value(json_object_get(payload,"queryId")))) {
						printf("[%s] received answer to query %s of type %s:\n Query:\n %s\n Result:\n %s \n", self->name,it->uid,result->type,it->msg->payload, result->payload);
						*rep = strdup(result->payload);
						query_t *dummy = it;
						it = zlist_next(self->query_list);
						zlist_remove(self->query_list,dummy);
						query_destroy(&dummy);
					    json_decref(payload);
						break;
					}
				}
			}
		} else if (streq (result->type, "mediator_uuid")) {
			// load the payload as json
			json_t *payload;
			json_error_t error;
			payload= json_loads(result->payload,0,&error);
			if(!payload) {
				printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
			} else {
				query_t *it = zlist_first(self->query_list);
				while (it != NULL) {
					if(json_object_get(payload,"UID") == 0) { // no queryIt in message, so we skip it here
						printf("Skipping mediator_uuid message without queryId\n");
						break;
					}
					if (streq(it->uid,json_string_value(json_object_get(payload,"UID")))) {
						printf("[%s] received answer to query %s of type %s:\n Query:\n %s\n Result:\n %s \n", self->name,it->uid,result->type,it->msg->payload, result->payload);
						*rep = strdup(result->payload);
						query_t *dummy = it;
						it = zlist_next(self->query_list);
						zlist_remove(self->query_list,dummy);
						query_destroy(&dummy);
					    json_decref(payload);
						break;
					}
				}
			}
		} else {
			printf("[%s] Unknown msg type!\n",self->name);
		}
	} else {
		printf ("[%s] message could not be decoded\n", self->name);
	}
	destroy_message(result);
	zstr_free(&peerid);
	zstr_free(&name);
	zstr_free(&group);
	zstr_free(&message);
}

void handle_join (component_t *self, zmsg_t *msg) {
	assert (zmsg_size(msg) == 3);
	char *peerid = zmsg_popstr (msg);
	char *name = zmsg_popstr (msg);
	char *group = zmsg_popstr (msg);
	printf ("[%s] JOIN %s %s %s\n", self->name, peerid, name, group);
	zstr_free(&peerid);
	zstr_free(&name);
	zstr_free(&group);
}

void handle_evasive (component_t *self, zmsg_t *msg) {
	assert (zmsg_size(msg) == 2);
	char *peerid = zmsg_popstr (msg);
	char *name = zmsg_popstr (msg);
	printf ("[%s] EVASIVE %s %s\n", self->name, peerid, name);
	zstr_free(&peerid);
	zstr_free(&name);
}


bool get_root_node_id(component_t *self, char** root_id) {
	assert(self);
	*root_id = NULL;
	char *msg;
	char *reply;

	// e.g.
//	{
//	  "@worldmodeltype": "RSGQuery",
//	  "query": "GET_ROOT_NODE"
//	}

	json_t *getRootNodeMsg = json_object();
	json_object_set_new(getRootNodeMsg, "@worldmodeltype", json_string("RSGQuery"));
	json_object_set_new(getRootNodeMsg, "query", json_string("GET_ROOT_NODE"));

	/* Send message and wait for reply */
	msg = encode_json_message(self, getRootNodeMsg);
	shout_message(self, msg);
	reply = wait_for_reply(self, msg, self->timeout);
	printf("#########################################\n");
	printf("[%s] Got reply for get_root_node_id: %s \n", self->name, reply);

	/* Parse reply */
    json_error_t error;
	json_t* rootNodeIdReply = json_loads(reply, 0, &error);


	json_t* querySuccessMsg = NULL;
	if(!rootNodeIdReply) {
		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
	} else {
		querySuccessMsg = json_object_get(rootNodeIdReply, "updateSuccess");
		char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
		printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
		free(dump);
	}
	bool querySuccess = false;
	free(msg);
	free(reply);
	json_t* rootNodeIdAsJSON = 0;
	if (querySuccessMsg) {
		querySuccess = json_is_true(querySuccessMsg);
	}

	json_t* rootIdMsg = json_object_get(rootNodeIdReply, "rootId");
	if (rootIdMsg) {
		*root_id = strdup(json_string_value(rootIdMsg));
		printf("[%s] get_root_node_id ID is: %s \n", self->name, *root_id);
	} else {
		querySuccess = false;
	}

	/* Clean up */
	json_decref(rootNodeIdReply);
	json_decref(getRootNodeMsg);

	return querySuccess;
}

bool get_gis_origin_id(component_t *self, char** origin_id) {
	assert(self);
//	*origin_id = NULL;
	return get_node_by_attribute(self, origin_id, "gis:origin", "wgs84");
}

bool get_observations_group_id(component_t *self, char** observations_id) {
	assert(self);
//	*observations_id = NULL;
	return get_node_by_attribute(self, observations_id, "name", "observations"); //TODO only search in subgraph of local root node
}

bool get_node_by_attribute(component_t *self, char** node_id, const char* key, const char* value) {
	assert(self);
	*node_id = NULL;
	char *msg;
	char *reply;

	// e.g.
	//    {
	//      "@worldmodeltype": "RSGQuery",
	//      "query": "GET_NODES",
	//      "attributes": [
	//          {"key": "name", "value": "observations"},
	//      ]
	//    }

	json_t *getNodeMsg = json_object();
	json_object_set_new(getNodeMsg, "@worldmodeltype", json_string("RSGQuery"));
	json_object_set_new(getNodeMsg, "query", json_string("GET_NODES"));
	json_t* nodeAttribute = json_object();
	json_object_set_new(nodeAttribute, "key", json_string(key));
	json_object_set_new(nodeAttribute, "value", json_string(value));
	json_t* originAttributes = json_array();
	json_array_append_new(originAttributes, nodeAttribute);
	json_object_set_new(getNodeMsg, "attributes", originAttributes);

	/* Send message and wait for reply */
	msg = encode_json_message(self, getNodeMsg);
	shout_message(self, msg);
	reply = wait_for_reply(self, msg, self->timeout);
	printf("#########################################\n");
	printf("[%s] Got reply for get_node_by_attribute: %s \n", self->name, reply);

	/* Parse reply */
    json_error_t error;
	json_t* nodeIdReply = json_loads(reply, 0, &error);
	free(msg);
	free(reply);
	json_t* nodeIdAsJSON = 0;
	json_t* array = json_object_get(nodeIdReply, "ids");
	if (array) {
		printf("[%s] result array found: \n", self->name);
		if( json_array_size(array) > 0 ) {
			nodeIdAsJSON = json_array_get(array, 0);
			*node_id = strdup(json_string_value(nodeIdAsJSON));
			printf("[%s] get_node_by_attribute ID is: %s \n", self->name, *node_id);
		} else {
			json_decref(nodeIdReply);
			json_decref(getNodeMsg);
			return false;
		}
	} else {
		json_decref(nodeIdReply);
		json_decref(getNodeMsg);
		return false;
	}

	/* Clean up */
	json_decref(nodeIdReply);
	json_decref(getNodeMsg);

	return true;
}

bool get_node_by_attribute_in_subgrapgh(component_t *self, char** node_id, const char* key, const char* value,  const char* subgraph_id) {
	assert(self);
	*node_id = NULL;
	char *msg;
	char *reply;

	// e.g.
	//    {
	//      "@worldmodeltype": "RSGQuery",
	//      "query": "GET_NODES",
	//      "subgraphId": "b0890bef-59fa-42f5-b195-cf5e28240d7d",
	//      "attributes": [
	//          {"key": "name", "value": "observations"},
	//      ]
	//    }

	json_t *getNodeMsg = json_object();
	json_object_set_new(getNodeMsg, "@worldmodeltype", json_string("RSGQuery"));
	json_object_set_new(getNodeMsg, "query", json_string("GET_NODES"));
	json_object_set_new(getNodeMsg, "subgraphId", json_string(subgraph_id));
	json_t* nodeAttribute = json_object();
	json_object_set_new(nodeAttribute, "key", json_string(key));
	json_object_set_new(nodeAttribute, "value", json_string(value));
	json_t* originAttributes = json_array();
	json_array_append_new(originAttributes, nodeAttribute);
	json_object_set_new(getNodeMsg, "attributes", originAttributes);

	/* Send message and wait for reply */
	msg = encode_json_message(self, getNodeMsg);
	shout_message(self, msg);
	reply = wait_for_reply(self, msg, self->timeout);
	printf("#########################################\n");
	printf("[%s] Got reply for get_node_by_attribute: %s \n", self->name, reply);

	/* Parse reply */
    json_error_t error;
	json_t* nodeIdReply = json_loads(reply, 0, &error);
	free(msg);
	free(reply);
	json_t* nodeIdAsJSON = 0;
	json_t* array = json_object_get(nodeIdReply, "ids");
	if (array) {
		printf("[%s] result array found: \n", self->name);
		if( json_array_size(array) > 0 ) {
			nodeIdAsJSON = json_array_get(array, 0);
			*node_id = strdup(json_string_value(nodeIdAsJSON));
			printf("[%s] get_node_by_attribute ID is: %s \n", self->name, *node_id);
		} else {
			json_decref(nodeIdReply);
			json_decref(getNodeMsg);
			return false;
		}
	} else {
		json_decref(nodeIdReply);
		json_decref(getNodeMsg);
		return false;
	}

	/* Clean up */
	json_decref(nodeIdReply);
	json_decref(getNodeMsg);

	return true;
}

bool add_geopose_to_node(component_t *self, const char* node_id, const char** new_geopose_id, double* transform_matrix, double utc_time_stamp_in_mili_sec, const char* key, const char* value) {
	assert(self);
	*new_geopose_id = NULL;
	char *msg;
	char *reply;

	char* originId = 0;
    if(!get_gis_origin_id(self, &originId)) {
    	printf("[%s] [ERROR] Cannot get origin Id \n", self->name);
    	return false;
    }
	printf("[%s] add_geopose_to_node origin Id = %s \n", self->name, originId);

	zuuid_t *uuid = zuuid_new ();
	assert(uuid);
	*new_geopose_id = zuuid_str_canonical(uuid);

	/*
	 * Pose
	 */
	//    newTransformMsg = {
	//      "@worldmodeltype": "RSGUpdate",
	//      "operation": "CREATE",
	//      "node": {
	//        "@graphtype": "Connection",
	//        "@semanticContext":"Transform",
	//        "id": tfId,
	//        "attributes": [
	//          {"key": "tf:type", "value": "wgs84"}
	//        ],
	//        "sourceIds": [
	//          originId,
	//        ],
	//        "targetIds": [
	//          imageNodeId,
	//        ],
	//        "history" : [
	//          {
	//            "stamp": {
	//              "@stamptype": "TimeStampDate",
	//              "stamp": currentTimeStamp,
	//            },
	//            "transform": {
	//              "type": "HomogeneousMatrix44",
	//                "matrix": [
	//                  [1,0,0,x],
	//                  [0,1,0,y],
	//                  [0,0,1,z],
	//                  [0,0,0,1]
	//                ],
	//                "unit": "latlon"
	//            }
	//          }
	//        ],
	//      },
	//      "parentId": originId,
	//    }

	// top level message
	json_t *newTfNodeMsg = json_object();
	json_object_set_new(newTfNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
	json_object_set_new(newTfNodeMsg, "operation", json_string("CREATE"));
	json_object_set_new(newTfNodeMsg, "parentId", json_string(originId));
	json_t *newTfConnection = json_object();
	json_object_set_new(newTfConnection, "@graphtype", json_string("Connection"));
	json_object_set_new(newTfConnection, "@semanticContext", json_string("Transform"));
	zuuid_t *poseUuid = zuuid_new ();
	json_object_set_new(newTfConnection, "id", json_string(*new_geopose_id));
	// Attributes
	json_t *poseAttribute = json_object();
	json_object_set_new(poseAttribute, "key", json_string("tf:type"));
	json_object_set_new(poseAttribute, "value", json_string("wgs84"));
	json_t *poseAttributes = json_array();
	json_array_append_new(poseAttributes, poseAttribute);
	if((key != NULL) && (value != NULL)) { // Optionally append a second generic attribute
		json_t *poseAttribute2 = json_object();
		json_object_set_new(poseAttribute2, "key", json_string(key));
		json_object_set_new(poseAttribute2, "value", json_string(value));
		json_array_append_new(poseAttributes, poseAttribute2);
	}
	json_object_set_new(newTfConnection, "attributes", poseAttributes);
	// sourceIds
	json_t *sourceIds = json_array();
	json_array_append_new(sourceIds, json_string(originId)); // ID of origin node
	json_object_set_new(newTfConnection, "sourceIds", sourceIds);
	// sourceIds
	json_t *targetIds = json_array();
	json_array_append_new(targetIds, json_string(node_id)); // ID of node that we just have created before
	json_object_set_new(newTfConnection, "targetIds", targetIds);

	// history
	json_t *history = json_array();
	json_t *stampedPose = json_object();
	json_t *stamp = json_object();
	json_t *pose = json_object();

	// stamp
	json_object_set_new(stamp, "@stamptype", json_string("TimeStampUTCms"));
	json_object_set_new(stamp, "stamp", json_real(utc_time_stamp_in_mili_sec));

	/* column-major layout:
	 * 0 4 8  12
	 * 1 5 9  13
	 * 2 6 10 14
	 * 3 7 11 15
	 *
	 *  <=>
	 *
	 * r11 r12 r13  x
	 * r21 r22 r23  y
	 * r31 r32 r33  z
	 * 3    7   11  15
	 */

	//pose
	json_object_set_new(pose, "type", json_string("HomogeneousMatrix44"));
	json_object_set_new(pose, "unit", json_string("latlon"));
	json_t *matrix = json_array();
	json_t *row0 = json_array();
	json_array_append_new(row0, json_real(transform_matrix[0]));
	json_array_append_new(row0, json_real(transform_matrix[4]));
	json_array_append_new(row0, json_real(transform_matrix[8]));
	json_array_append_new(row0, json_real(transform_matrix[12]));
	json_t *row1 = json_array();
	json_array_append_new(row1, json_real(transform_matrix[1]));
	json_array_append_new(row1, json_real(transform_matrix[5]));
	json_array_append_new(row1, json_real(transform_matrix[9]));
	json_array_append_new(row1, json_real(transform_matrix[13]));
	json_t *row2 = json_array();
	json_array_append_new(row2, json_real(transform_matrix[2]));
	json_array_append_new(row2, json_real(transform_matrix[6]));
	json_array_append_new(row2, json_real(transform_matrix[10]));
	json_array_append_new(row2, json_real(transform_matrix[14]));
	json_t *row3 = json_array();
	json_array_append_new(row3, json_real(transform_matrix[3]));
	json_array_append_new(row3, json_real(transform_matrix[7]));
	json_array_append_new(row3, json_real(transform_matrix[11]));
	json_array_append_new(row3, json_real(transform_matrix[15]));
	json_array_append_new(matrix, row0);
	json_array_append_new(matrix, row1);
	json_array_append_new(matrix, row2);
	json_array_append_new(matrix, row3);

	json_object_set_new(pose, "matrix", matrix);

	json_object_set_new(stampedPose, "stamp", stamp);
	json_object_set_new(stampedPose, "transform", pose);
	json_array_append_new(history, stampedPose);
	json_object_set_new(newTfConnection, "history", history);
	json_object_set_new(newTfNodeMsg, "node", newTfConnection);

	/* Send message and wait for reply */
	msg = encode_json_message(self, newTfNodeMsg);
	shout_message(self, msg);
	reply = wait_for_reply(self, msg, self->timeout);
	printf("#########################################\n");
	printf("[%s] Got reply for pose: %s \n", self->name, reply);

	/* Parse reply */
	json_error_t error;
	json_t* tfReply = json_loads(reply, 0, &error);
	json_t* querySuccessMsg = NULL;
	if(!tfReply) {
		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
	} else {
		querySuccessMsg = json_object_get(tfReply, "updateSuccess");
		char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
		printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
		free(dump);
	}
	bool querySuccess = false;
	if (querySuccessMsg) {
		querySuccess = json_is_true(querySuccessMsg);
	}

	/* Clean up */
	free(msg);
	free(reply);
	free(uuid);
	free(poseUuid);
	free(originId);
	json_decref(newTfNodeMsg);
	json_decref(tfReply);

	return querySuccess;
}

bool get_mediator_id(component_t *self, char** mediator_id) {
	assert(self);
	char* reply = NULL;

	// Generate message
	//    {
	//      "metamodel": "sherpa_mgs",
	//      "model": "http://kul/query_mediator_uuid.json",
	//      "type": "query_mediator_uuid",
	//		"payload":
	//		{
	//			"UID": 2147aba0-0d59-41ec-8531-f6787fe52b60
	//		}
	//    }
	json_t *getMediatorIDMsg = json_object();
	json_object_set_new(getMediatorIDMsg, "metamodel", json_string("sherpa_mgs"));
	json_object_set_new(getMediatorIDMsg, "model", json_string("http://kul/query_mediator_uuid.json"));
	json_object_set_new(getMediatorIDMsg, "type", json_string("query_mediator_uuid"));
	json_t* pl = json_object();
	zuuid_t *uuid = zuuid_new ();
	assert(uuid);
	json_object_set_new(pl, "UID", json_string(zuuid_str_canonical(uuid)));
	json_object_set_new(getMediatorIDMsg, "payload", pl);

	// add it to the query list
	json_msg_t *msg = (json_msg_t *) zmalloc (sizeof (json_msg_t));
	msg->metamodel = strdup("sherpa_mgs");
	msg->model = strdup("http://kul/query_mediator_uuid.json");
	msg->type = strdup("query_mediator_uuid");
	msg->payload = json_dumps(pl, JSON_ENCODE_ANY);
	query_t * q = query_new(zuuid_str_canonical(uuid), zyre_uuid(self->local), msg, NULL);
	zlist_append(self->query_list, q);
	free(uuid);

	char* ret = json_dumps(getMediatorIDMsg, JSON_ENCODE_ANY);
	printf("[%s] send_json_message: message = %s:\n", self->name, ret);
	json_decref(getMediatorIDMsg);

	/* Send message and wait for reply */
	shout_message(self, ret);
	reply = wait_for_reply(self, ret, self->timeout);
	if (reply==0) {
		printf("[%s] Received no reply for mediator_id query.\n", self->name);
		free(msg);
		free(ret);
		free(reply);
		return false;
	}
	//printf("[%s] Got reply for query_mediator_uuid: %s \n", self->name, reply);

	/* Parse reply */
    json_error_t error;
	json_t* rep = json_loads(reply, 0, &error);
//	free(msg); wait_for_reply handles this
	free(ret);
	free(reply);
	if(!rep) {
		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
		return false;
	}
	*mediator_id = strdup(json_string_value(json_object_get(rep, "remote")));
	json_decref(rep);
	if (!*mediator_id) {
		printf("Reply did not contain mediator ID.\n");
		return false;
	}

	return true;
}

bool add_victim(component_t *self, double* transform_matrix, double utc_time_stamp_in_mili_sec, char* author) {

	if (self == NULL) {
		return false;
		printf("[ERROR] Communication component is not yet initialized.\n");
	}

	char* msg;
	char* reply;

	/* Get observationGroupId */
	char* observationGroupId = 0;
    if(!get_observations_group_id(self, &observationGroupId)) {
    	printf("[%s] [ERROR] Cannot get observation group Id \n", self->name);
    	return false;
    }
	printf("[%s] observation Id = %s \n", self->name, observationGroupId);

    /*
     * Get the "origin" node. It is relevant to specify a new pose.
     */
	char* originId = 0;
    if(!get_gis_origin_id(self, &originId)) {
    	printf("[%s] [ERROR] Cannot get origin Id \n", self->name);
    	return false;
    }
	printf("[%s] origin Id = %s \n", self->name, originId);


	/*
	 * The actual "observation" node. Here for a victim.
	 */

	//    currentTimeStamp = datetime.datetime.now().strftime('%Y-%m-%dT%H:%M:%S')
	//    imageNodeId = str(uuid.uuid4())
	//    newImageNodeMsg = {
	//      "@worldmodeltype": "RSGUpdate",
	//      "operation": "CREATE",
	//      "node": {
	//        "@graphtype": "Node",
	//        "id": imageNodeId,
	//        "attributes": [
	//              {"key": "sherpa:observation_type", "value": "image"},
	//              {"key": "sherpa:uri", "value": URI},
	//              {"key": "sherpa:stamp", "value": currentTimeStamp},
	//              {"key": "sherpa:author", "value": author},
	//        ],
	//      },
	//      "parentId": observationGroupId,
	//    }

	// top level message
	json_t *newImageNodeMsg = json_object();
	json_object_set_new(newImageNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
	json_object_set_new(newImageNodeMsg, "operation", json_string("CREATE"));
	json_object_set_new(newImageNodeMsg, "parentId", json_string(observationGroupId));
	json_t *newImageNode = json_object();
	json_object_set_new(newImageNode, "@graphtype", json_string("Node"));
	zuuid_t *uuid = zuuid_new ();
	json_object_set_new(newImageNode, "id", json_string(zuuid_str_canonical(uuid)));

	// attributes
	json_t *newObservationAttributes = json_array();
	json_t *attribute1 = json_object();
	json_object_set_new(attribute1, "key", json_string("sherpa:observation_type"));
	json_object_set_new(attribute1, "value", json_string("victim"));
	json_array_append_new(newObservationAttributes, attribute1);
	//	json_t *attribute2 = json_object();
	//	json_object_set(attribute2, "key", json_string("sherpa:uri"));
	//	json_object_set(attribute2, "value", json_string("TODO"));
	//	json_array_append(attributes, attribute2);
	json_t *attribute3 = json_object();
	json_object_set_new(attribute3, "key", json_string("sherpa:stamp"));
	json_object_set_new(attribute3, "value", json_real(utc_time_stamp_in_mili_sec));
	json_array_append_new(newObservationAttributes, attribute3);
	json_t *attribute4 = json_object();
	json_object_set_new(attribute4, "key", json_string("sherpa:author"));
	json_object_set_new(attribute4, "value", json_string(author));
	json_array_append_new(newObservationAttributes, attribute4);


	json_object_set_new(newImageNode, "attributes", newObservationAttributes);
	json_object_set_new(newImageNodeMsg, "node", newImageNode);

	/* CReate message*/
	msg = encode_json_message(self, newImageNodeMsg);
	/* Send the message */
	shout_message(self, msg);
	/* Wait for a reply */
	reply = wait_for_reply(self, msg, self->timeout);
	/* Print reply */
	printf("#########################################\n");
	printf("[%s] Got reply: %s \n", self->name, reply);


	const char* poseId;
	bool succsess = add_geopose_to_node(self, zuuid_str_canonical(uuid), &poseId, transform_matrix, utc_time_stamp_in_mili_sec, 0, 0);


	/* Clean up */
	free(msg);
	free(reply);
	free(observationGroupId);
	free(originId);
	free(uuid);
	json_decref(newImageNodeMsg);

	return succsess;
}

bool add_image(component_t *self, double* transform_matrix, double utc_time_stamp_in_mili_sec, char* author, char* file_name) {
	if (self == NULL) {
		return false;
		printf("[ERROR] Communication component is not yet initialized.\n");
	}

	char* msg;
	char* reply;

	/* Get observationGroupId */
	char* observationGroupId = 0;
    if(!get_observations_group_id(self, &observationGroupId)) {
    	printf("[%s] [ERROR] Cannot get observation group Id \n", self->name);
    	return false;
    }
	printf("[%s] observation Id = %s \n", self->name, observationGroupId);

    /*
     * Get the "origin" node. It is relevant to specify a new pose.
     */
	char* originId = 0;
    if(!get_gis_origin_id(self, &originId)) {
    	printf("[%s] [ERROR] Cannot get origin Id \n", self->name);
    	return false;
    }
	printf("[%s] origin Id = %s \n", self->name, originId);

	/* get Mediator ID */
	char* mediator_uuid; //= "79346b2b-e0a1-4e04-a7c8-981828436357";
	if(!get_mediator_id(self, &mediator_uuid)) {
		printf("[%s] [ERROR] Cannot get Mediator ID. Is the Mediator started? \n", self->name);
		return false;
	}

    char uri[1024] = {0};
    snprintf(uri, sizeof(uri), "%s:%s", mediator_uuid, file_name);

	/*
	 * The actual "observation" node. Here for an image.
	 */

	//    currentTimeStamp = datetime.datetime.now().strftime('%Y-%m-%dT%H:%M:%S')
	//    imageNodeId = str(uuid.uuid4())
	//    newImageNodeMsg = {
	//      "@worldmodeltype": "RSGUpdate",
	//      "operation": "CREATE",
	//      "node": {
	//        "@graphtype": "Node",
	//        "id": imageNodeId,
	//        "attributes": [
	//              {"key": "sherpa:observation_type", "value": "image"},
	//              {"key": "sherpa:uri", "value": URI},
	//              {"key": "sherpa:stamp", "value": currentTimeStamp},
	//              {"key": "sherpa:author", "value": author},
	//        ],
	//      },
	//      "parentId": observationGroupId,
	//    }

	// top level message
	json_t *newImageNodeMsg = json_object();
	json_object_set_new(newImageNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
	json_object_set_new(newImageNodeMsg, "operation", json_string("CREATE"));
	json_object_set_new(newImageNodeMsg, "parentId", json_string(observationGroupId));
	json_t *newImageNode = json_object();
	json_object_set_new(newImageNode, "@graphtype", json_string("Node"));
	zuuid_t *uuid = zuuid_new ();
	json_object_set_new(newImageNode, "id", json_string(zuuid_str_canonical(uuid)));

	// attributes
	json_t *newObservationAttributes = json_array();
	json_t *attribute1 = json_object();
	json_object_set_new(attribute1, "key", json_string("sherpa:observation_type"));
	json_object_set_new(attribute1, "value", json_string("image"));
	json_array_append_new(newObservationAttributes, attribute1);
	json_t *attribute2 = json_object();
	json_object_set_new(attribute2, "key", json_string("sherpa:uri"));
	json_object_set_new(attribute2, "value", json_string(uri));
	json_array_append_new(newObservationAttributes, attribute2);
	json_t *attribute3 = json_object();
	json_object_set_new(attribute3, "key", json_string("sherpa:stamp"));
	json_object_set_new(attribute3, "value", json_real(utc_time_stamp_in_mili_sec));
	json_array_append_new(newObservationAttributes, attribute3);
	json_t *attribute4 = json_object();
	json_object_set_new(attribute4, "key", json_string("sherpa:author"));
	json_object_set_new(attribute4, "value", json_string(author));
	json_array_append_new(newObservationAttributes, attribute4);


	json_object_set_new(newImageNode, "attributes", newObservationAttributes);
	json_object_set_new(newImageNodeMsg, "node", newImageNode);

	/* CReate message*/
	msg = encode_json_message(self, newImageNodeMsg);
	/* Send the message */
	shout_message(self, msg);
	/* Wait for a reply */
	reply = wait_for_reply(self, msg, self->timeout);
	/* Print reply */
	printf("#########################################\n");
	printf("[%s] Got reply: %s \n", self->name, reply);


	const char* poseId;
	bool succsess = add_geopose_to_node(self, zuuid_str_canonical(uuid), &poseId, transform_matrix, utc_time_stamp_in_mili_sec, 0, 0);


	/* Clean up */
	free(msg);
	free(reply);
	free(observationGroupId);
	free(originId);
	free(uuid);
	free(mediator_uuid);
	json_decref(newImageNodeMsg);

	return succsess;
}

#if 0
bool add_artva(component_t *self, double* transform_matrix, double artva0, double artva1, double artva2, double artva3,
		double utc_time_stamp_in_mili_sec, char* author) {

	if (self == NULL) {
		return false;
		printf("[ERROR] Communication component is not yet initialized.\n");
	}

	char* msg;
	char* reply;

	/* Get observationGroupId */
	char* observationGroupId = 0;
    if(!get_observations_group_id(self, &observationGroupId)) {
    	printf("[%s] [ERROR] Cannot get observation group Id \n", self->name);
    	return false;
    }
	printf("[%s] observation Id = %s \n", self->name, observationGroupId);

    /*
     * Get the "origin" node. It is relevant to specify a new pose.
     */
	char* originId = 0;
    if(!get_gis_origin_id(self, &originId)) {
    	printf("[%s] [ERROR] Cannot get origin Id \n", self->name);
    	return false;
    }
	printf("[%s] origin Id = %s \n", self->name, originId);

	/*
	 * The actual "observation" node. Here for an an artva signal.
	 */

	//    currentTimeStamp = datetime.datetime.now().strftime('%Y-%m-%dT%H:%M:%S')
	//    imageNodeId = str(uuid.uuid4())
	//    newImageNodeMsg = {
	//      "@worldmodeltype": "RSGUpdate",
	//      "operation": "CREATE",
	//      "node": {
	//        "@graphtype": "Node",
	//        "id": imageNodeId,
	//        "attributes": [
	//              {"key": "sherpa:observation_type", "value": "artva"},
    //              {"key": "sherpa:artva_signal0", "value": "77"},
    //              {"key": "sherpa:artva_signal1", "value": "12"},
    //              {"key": "sherpa:artva_signal2", "value": "0"},
    //              {"key": "sherpa:artva_signal3", "value": "0"},
	// OPTIONAL:
	//              {"key": "sherpa:stamp", "value": currentTimeStamp},
	//              {"key": "sherpa:author", "value": author},
	//        ],
	//      },
	//      "parentId": observationGroupId,
	//    }

	// top level message
	json_t *newARTVANodeMsg = json_object();
	json_object_set_new(newARTVANodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
	json_object_set_new(newARTVANodeMsg, "operation", json_string("CREATE"));
	json_object_set_new(newARTVANodeMsg, "parentId", json_string(observationGroupId));
	json_t *newImageNode = json_object();
	json_object_set_new(newImageNode, "@graphtype", json_string("Node"));
	zuuid_t *uuid = zuuid_new ();
	json_object_set_new(newImageNode, "id", json_string(zuuid_str_canonical(uuid)));

	// attributes
	json_t *newObservationAttributes = json_array();
	json_t *attribute1 = json_object();
	json_object_set_new(attribute1, "key", json_string("sherpa:observation_type"));
	json_object_set_new(attribute1, "value", json_string("artva"));
	json_array_append_new(newObservationAttributes, attribute1);
	json_t *attribute2a = json_object();
	json_object_set_new(attribute2a, "key", json_string("sherpa:artva_signal0"));
	json_object_set_new(attribute2a, "value", json_real(artva0));
	json_array_append_new(newObservationAttributes, attribute2a);
	json_t *attribute2b = json_object();
	json_object_set_new(attribute2b, "key", json_string("sherpa:artva_signal1"));
	json_object_set_new(attribute2b, "value", json_real(artva1));
	json_array_append_new(newObservationAttributes, attribute2b);
	json_t *attribute2c = json_object();
	json_object_set_new(attribute2c, "key", json_string("sherpa:artva_signal2"));
	json_object_set_new(attribute2c, "value", json_real(artva2));
	json_array_append_new(newObservationAttributes, attribute2c);
	json_t *attribute2d = json_object();
	json_object_set_new(attribute2d, "key", json_string("sherpa:artva_signal3"));
	json_object_set_new(attribute2d, "value", json_real(artva3));
	json_array_append_new(newObservationAttributes, attribute2d);
	json_t *attribute3 = json_object();
	json_object_set_new(attribute3, "key", json_string("sherpa:stamp"));
	json_object_set_new(attribute3, "value", json_real(utc_time_stamp_in_mili_sec));
	json_array_append_new(newObservationAttributes, attribute3);
	json_t *attribute4 = json_object();
	json_object_set_new(attribute4, "key", json_string("sherpa:author"));
	json_object_set_new(attribute4, "value", json_string(author));
	json_array_append_new(newObservationAttributes, attribute4);


	json_object_set_new(newImageNode, "attributes", newObservationAttributes);
	json_object_set_new(newARTVANodeMsg, "node", newImageNode);

	/* CReate message*/
	msg = encode_json_message(self, newARTVANodeMsg);
	/* Send the message */
	shout_message(self, msg);
	/* Wait for a reply */
	reply = wait_for_reply(self, msg, self->timeout);
	/* Print reply */
	printf("#########################################\n");
	printf("[%s] Got reply: %s \n", self->name, reply);


	const char* poseId;
	bool succsess = add_geopose_to_node(self, zuuid_str_canonical(uuid), &poseId, transform_matrix, utc_time_stamp_in_mili_sec, 0, 0);


	/* Clean up */
	free(msg);
	free(reply);
	free(observationGroupId);
	free(originId);
	free(uuid);
	json_decref(newARTVANodeMsg);

	return succsess;
}
#endif

bool add_artva_measurement(component_t *self, artva_measurement measurement, char* author) {
	if (self == NULL) {
		return false;
		printf("[ERROR] Communication component is not yet initialized.\n");
	}

	char* msg;
	char* reply;
    json_error_t error;


	/* Get root ID to restrict search to subgraph of local SWM */
	char* scope_id = 0;
	if (!get_node_by_attribute(self, &scope_id, "sherpa:agent_name", author)) { // only search within the scope of this agent
		printf("[%s] [ERROR] Cannot get cope Id \n", self->name);
		return false;
	}

	/* prepare payload */
	// attributes
	json_t* attributes = json_array();
	json_t* attribute1 = json_object();
	json_object_set_new(attribute1, "key", json_string("sherpa:observation_type"));
	json_object_set_new(attribute1, "value", json_string("artva"));
	json_array_append_new(attributes, attribute1);

	json_t* attribute2 = json_object();
	json_object_set_new(attribute2, "key", json_string("sherpa:artva_signal0"));
	json_object_set_new(attribute2, "value", json_integer(measurement.signal0));
	json_array_append_new(attributes, attribute2);

	json_t* attribute3 = json_object();
	json_object_set_new(attribute3, "key", json_string("sherpa:artva_signal1"));
	json_object_set_new(attribute3, "value", json_integer(measurement.signal1));
	json_array_append_new(attributes, attribute3);

	json_t* attribute4 = json_object();
	json_object_set_new(attribute4, "key", json_string("sherpa:artva_signal2"));
	json_object_set_new(attribute4, "value", json_integer(measurement.signal2));
	json_array_append_new(attributes, attribute4);

	json_t* attribute5 = json_object();
	json_object_set_new(attribute5, "key", json_string("sherpa:artva_signal3"));
	json_object_set_new(attribute5, "value", json_integer(measurement.signal3));
	json_array_append_new(attributes, attribute5);

	json_t* attribute6 = json_object();
	json_object_set_new(attribute6, "key", json_string("sherpa:artva_angle0"));
	json_object_set_new(attribute6, "value", json_integer(measurement.angle0));
	json_array_append_new(attributes, attribute6);

	json_t* attribute7 = json_object();
	json_object_set_new(attribute7, "key", json_string("sherpa:artva_angle1"));
	json_object_set_new(attribute7, "value", json_integer(measurement.angle1));
	json_array_append_new(attributes, attribute7);

	json_t* attribute8 = json_object();
	json_object_set_new(attribute8, "key", json_string("sherpa:artva_angle2"));
	json_object_set_new(attribute8, "value", json_integer(measurement.angle2));
	json_array_append_new(attributes, attribute8);

	json_t* attribute9 = json_object();
	json_object_set_new(attribute9, "key", json_string("sherpa:artva_angle3"));
	json_object_set_new(attribute9, "value", json_integer(measurement.angle3));
	json_array_append_new(attributes, attribute9);

	char* artvaId = 0;
	if (!get_node_by_attribute_in_subgrapgh(self, &artvaId, "sherpa:observation_type", "artva", scope_id)) { // measurement node does not exist yet, so we will add it here

		/* Get observationGroupId */
		char* observationGroupId = 0;
		if(!get_observations_group_id(self, &observationGroupId)) {
			printf("[%s] [ERROR] Cannot get observation group Id \n", self->name);
			return false;
		}
		printf("[%s] observation Id = %s \n", self->name, observationGroupId);


		json_t *newArtvaMeasurementMsg = json_object();
		json_object_set_new(newArtvaMeasurementMsg, "@worldmodeltype", json_string("RSGUpdate"));
		json_object_set_new(newArtvaMeasurementMsg, "operation", json_string("CREATE"));
		json_object_set_new(newArtvaMeasurementMsg, "parentId", json_string(observationGroupId));
		json_t *newArtvaNode = json_object();
		json_object_set_new(newArtvaNode, "@graphtype", json_string("Node"));
		zuuid_t *uuid = zuuid_new ();
		char* artvaNodeId = zuuid_str_canonical(uuid);
		json_object_set_new(newArtvaNode, "id", json_string(artvaNodeId));

		// attributes see above

		json_object_set_new(newArtvaNode, "attributes", attributes);
		json_object_set_new(newArtvaMeasurementMsg, "node", newArtvaNode);

		/* CReate message*/
		msg = encode_json_message(self, newArtvaMeasurementMsg);
		/* Send the message */
		shout_message(self, msg);
		/* Wait for a reply */
		reply = wait_for_reply(self);
		/* Print reply */
		printf("#########################################\n");
		printf("[%s] Got reply: %s \n", self->name, reply);

		/* Parse reply */
		json_t* newArtvaNodeReply = json_loads(reply, 0, &error);
		json_t* querySuccessMsg = json_object_get(newArtvaNodeReply, "updateSuccess");
		bool querySuccess = false;
		char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
		printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
		free(dump);

		if (querySuccessMsg) {
			querySuccess = json_is_true(querySuccessMsg);
		}

		json_decref(newArtvaMeasurementMsg);
		json_decref(newArtvaNodeReply);
		free(msg);
		free(reply);
		free(artvaNodeId);
		free(uuid);

		if(!querySuccess) {
			printf("[%s] [ERROR] Can not add battery node for agent.\n", self->name);
			return false;
		}

		return true;
	} // it exists, so just update it

	// if it exists already, just UPDATE the attributes

		//        batteryUpdateMsg = {
		//          "@worldmodeltype": "RSGUpdate",
		//          "operation": "UPDATE_ATTRIBUTES",
		//          "node": {
		//            "@graphtype": "Node",
		//            "id": self.battery_uuid,
		//            "attributes": [
		//                  {"key": "sensor:battery_voltage", "value": self.battery_voltage},
		//            ],
		//           },
		//        }

	json_t *updateArtvaNodeMsg = json_object();
	json_object_set_new(updateArtvaNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
	json_object_set_new(updateArtvaNodeMsg, "operation", json_string("UPDATE_ATTRIBUTES"));
	json_t *updateArtvaNode = json_object();
	json_object_set_new(updateArtvaNode, "@graphtype", json_string("Node"));
	json_object_set_new(updateArtvaNode, "id", json_string(artvaId));

	// attributes see above

	json_object_set_new(updateArtvaNode, "attributes", attributes);
	json_object_set_new(updateArtvaNodeMsg, "node", updateArtvaNode);

	/* CReate message*/
	msg = encode_json_message(self, updateArtvaNodeMsg);
	/* Send the message */
	shout_message(self, msg);
	/* Wait for a reply */
	reply = wait_for_reply(self);
	/* Print reply */
	printf("#########################################\n");
	printf("[%s] Got reply: %s \n", self->name, reply);

	/* Parse reply */
	json_t* updateArtvaReply = json_loads(reply, 0, &error);
	json_t* querySuccessMsg = json_object_get(updateArtvaReply, "updateSuccess");
	bool updateSuccess = false;
	char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
	printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
	free(dump);

	if (querySuccessMsg) {
		//updateSuccess = json_is_true(querySuccessMsg);
		updateSuccess = true; //FIXME updates of same values are not yet supported
	}

	json_decref(updateArtvaNodeMsg);
	json_decref(updateArtvaReply);
	free(msg);
	free(reply);

	return updateSuccess;

}

bool add_wasp_status(component_t *self, wasp_status status, char* author) {
	if (self == NULL) {
		return false;
		printf("[ERROR] Communication component is not yet initialized.\n");
	}

	char* msg;
	char* reply;
    json_error_t error;


	/* Get root ID to restrict search to subgraph of local SWM */
	char* scope_id = 0;
	if (!get_node_by_attribute(self, &scope_id, "sherpa:agent_name", author)) { // only search within the scope of this agent
		printf("[%s] [ERROR] Cannot get cope Id \n", self->name);
		return false;
	}

	/* prepare payload */
	// attributes
	json_t* attributes = json_array();
	json_t* attribute1 = json_object();
	json_object_set_new(attribute1, "key", json_string("sherpa:status_type"));
	json_object_set_new(attribute1, "value", json_string("wasp"));
	json_array_append_new(attributes, attribute1);

	json_t* attribute2 = json_object();
	json_object_set_new(attribute2, "key", json_string("sherpa:wasp_flight_state"));
	json_object_set_new(attribute2, "value", json_string(status.flight_state));
	json_array_append_new(attributes, attribute2);

	json_t* attribute3 = json_object();
	json_object_set_new(attribute3, "key", json_string("sherpa:wasp_on_box"));
	json_object_set_new(attribute3, "value", json_string(status.wasp_on_box));
	json_array_append_new(attributes, attribute3);

	char* waspStatusId = 0;
	if (!get_node_by_attribute_in_subgrapgh(self, &waspStatusId, "sherpa:status_type", "wasp", scope_id)) { // measurement node does not exist yet, so we will add it here

		/* Get observationGroupId */
		char* observationGroupId = 0;
		if(!get_observations_group_id(self, &observationGroupId)) {
			printf("[%s] [ERROR] Cannot get observation group Id \n", self->name);
			return false;
		}
		printf("[%s] observation Id = %s \n", self->name, observationGroupId);


		json_t *newWaspStatusMsg = json_object();
		json_object_set_new(newWaspStatusMsg, "@worldmodeltype", json_string("RSGUpdate"));
		json_object_set_new(newWaspStatusMsg, "operation", json_string("CREATE"));
		json_object_set_new(newWaspStatusMsg, "parentId", json_string(observationGroupId));
		json_t *newWaspNode = json_object();
		json_object_set_new(newWaspNode, "@graphtype", json_string("Node"));
		zuuid_t *uuid = zuuid_new ();
		char* waspNodeId = zuuid_str_canonical(uuid);
		json_object_set_new(newWaspNode, "id", json_string(waspNodeId));

		// attributes see above

		json_object_set_new(newWaspNode, "attributes", attributes);
		json_object_set_new(newWaspStatusMsg, "node", newWaspNode);

		/* CReate message*/
		msg = encode_json_message(self, newWaspStatusMsg);
		/* Send the message */
		shout_message(self, msg);
		/* Wait for a reply */
		reply = wait_for_reply(self);
		/* Print reply */
		printf("#########################################\n");
		printf("[%s] Got reply: %s \n", self->name, reply);

		/* Parse reply */
		json_t* newWaspStatusNodeReply = json_loads(reply, 0, &error);
		json_t* querySuccessMsg = json_object_get(newWaspStatusNodeReply, "updateSuccess");
		bool querySuccess = false;
		char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
		printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
		free(dump);

		if (querySuccessMsg) {
			querySuccess = json_is_true(querySuccessMsg);
		}

		json_decref(newWaspStatusMsg);
		json_decref(newWaspStatusNodeReply);
		free(msg);
		free(reply);
		free(waspNodeId);
		free(uuid);

		if(!querySuccess) {
			printf("[%s] [ERROR] Can not add wasp status node for agent.\n", self->name);
			return false;
		}

		return true;
	} // it exists, so just update it

	// if it exists already, just UPDATE the attributes

		//        batteryUpdateMsg = {
		//          "@worldmodeltype": "RSGUpdate",
		//          "operation": "UPDATE_ATTRIBUTES",
		//          "node": {
		//            "@graphtype": "Node",
		//            "id": self.battery_uuid,
		//            "attributes": [
		//                  {"key": "sensor:battery_voltage", "value": self.battery_voltage},
		//            ],
		//           },
		//        }

	json_t *updateWaspStatusNodeMsg = json_object();
	json_object_set_new(updateWaspStatusNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
	json_object_set_new(updateWaspStatusNodeMsg, "operation", json_string("UPDATE_ATTRIBUTES"));
	json_t *updateWaspNode = json_object();
	json_object_set_new(updateWaspNode, "@graphtype", json_string("Node"));
	json_object_set_new(updateWaspNode, "id", json_string(waspStatusId));

	// attributes see above

	json_object_set_new(updateWaspNode, "attributes", attributes);
	json_object_set_new(updateWaspStatusNodeMsg, "node", updateWaspNode);

	/* CReate message*/
	msg = encode_json_message(self, updateWaspStatusNodeMsg);
	/* Send the message */
	shout_message(self, msg);
	/* Wait for a reply */
	reply = wait_for_reply(self);
	/* Print reply */
	printf("#########################################\n");
	printf("[%s] Got reply: %s \n", self->name, reply);

	/* Parse reply */
	json_t* updateWaspStatusReply = json_loads(reply, 0, &error);
	json_t* querySuccessMsg = json_object_get(updateWaspStatusReply, "updateSuccess");
	bool updateSuccess = false;
	char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
	printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
	free(dump);

	if (querySuccessMsg) {
		//updateSuccess = json_is_true(querySuccessMsg);
		updateSuccess = true; //FIXME updates of same values are not yet supported
	}

	json_decref(updateWaspStatusNodeMsg);
	json_decref(updateWaspStatusReply);
	free(msg);
	free(reply);

	return updateSuccess;
}

bool add_battery(component_t *self, double battery_voltage, char* battery_status,  double utc_time_stamp_in_mili_sec, char* author) {

	if (self == NULL) {
		return false;
		printf("[ERROR] Communication component is not yet initialized.\n");
	}

	char* msg;
	char* reply;

	/* Get root ID to restrict search to subgraph of local SWM */
	char* root_id = 0;
	if (!get_root_node_id(self, &root_id)) {
		printf("[%s] [ERROR] Cannot get root  Id \n", self->name);
		return false;
	}

	char* batteryId = 0;
	if (!get_node_by_attribute_in_subgrapgh(self, &batteryId, "sherpa:observation_type", "battery", root_id)) { // battery does not exist yet, so we will add it here

		/* Get observationGroupId */
		char* observationGroupId = 0;
		if(!get_observations_group_id(self, &observationGroupId)) {
			printf("[%s] [ERROR] Cannot get observation group Id \n", self->name);
			return false;
		}
		printf("[%s] observation Id = %s \n", self->name, observationGroupId);


		json_t *newBatteryNodeMsg = json_object();
		json_object_set_new(newBatteryNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
		json_object_set_new(newBatteryNodeMsg, "operation", json_string("CREATE"));
		json_object_set_new(newBatteryNodeMsg, "parentId", json_string(observationGroupId));
		json_t *newAgentNode = json_object();
		json_object_set_new(newAgentNode, "@graphtype", json_string("Node"));
		zuuid_t *uuid = zuuid_new ();
		const char* batteryId = zuuid_str_canonical(uuid);
		json_object_set_new(newAgentNode, "id", json_string(batteryId));

		// attributes
		json_t* attributes = json_array();
		json_t* attribute1 = json_object();
		json_object_set_new(attribute1, "key", json_string("sherpa:observation_type"));
		json_object_set_new(attribute1, "value", json_string("battery"));
		json_array_append_new(attributes, attribute1);

		json_t* attribute2 = json_object();
		json_object_set_new(attribute2, "key", json_string("sherpa:battery_voltage"));
		json_object_set_new(attribute2, "value", json_real(battery_voltage));
		json_array_append_new(attributes, attribute2);

		json_t* attribute3 = json_object();
		json_object_set_new(attribute3, "key", json_string("sherpa:battery_status"));
		json_object_set_new(attribute3, "value", json_string(battery_status));
		json_array_append_new(attributes, attribute3);

		json_object_set_new(newAgentNode, "attributes", attributes);
		json_object_set_new(newBatteryNodeMsg, "node", newAgentNode);

		/* CReate message*/
		msg = encode_json_message(self, newBatteryNodeMsg);
		/* Send the message */
		shout_message(self, msg);
		/* Wait for a reply */
		reply = wait_for_reply(self, msg, self->timeout);
		/* Print reply */
		printf("#########################################\n");
		printf("[%s] Got reply: %s \n", self->name, reply);

		/* Parse reply */
		json_error_t error;
		json_t* newBatteryReply = json_loads(reply, 0, &error);
		json_t* querySuccessMsg = NULL;
		if(!newBatteryReply) {
			printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
		} else {
			querySuccessMsg = json_object_get(newBatteryReply, "updateSuccess");
			char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
			printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
			free(dump);
		}
		bool querySuccess = false;

		if (querySuccessMsg) {
			querySuccess = json_is_true(querySuccessMsg);
		}

		json_decref(newBatteryNodeMsg);
		json_decref(newBatteryReply);
		free(msg);
		free(reply);
		free(uuid);

		if(!querySuccess) {
			printf("[%s] [ERROR] Can not add battery node for agent.\n", self->name);
			return false;
		}

		return true;
	}

	// if it exists already, just UPDATE the attributes

		//        batteryUpdateMsg = {
		//          "@worldmodeltype": "RSGUpdate",
		//          "operation": "UPDATE_ATTRIBUTES",
		//          "node": {
		//            "@graphtype": "Node",
		//            "id": self.battery_uuid,
		//            "attributes": [
		//                  {"key": "sensor:battery_voltage", "value": self.battery_voltage},
		//            ],
		//           },
		//        }

	json_t *updateBatteryNodeMsg = json_object();
	json_object_set_new(updateBatteryNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
	json_object_set_new(updateBatteryNodeMsg, "operation", json_string("UPDATE_ATTRIBUTES"));
	json_t *newAgentNode = json_object();
	json_object_set_new(newAgentNode, "@graphtype", json_string("Node"));
	json_object_set_new(newAgentNode, "id", json_string(batteryId));

	// attributes
	json_t* attributes = json_array();
	json_t* attribute1 = json_object();
	json_object_set_new(attribute1, "key", json_string("sherpa:observation_type"));
	json_object_set_new(attribute1, "value", json_string("battery"));
	json_array_append_new(attributes, attribute1);

	json_t* attribute2 = json_object();
	json_object_set_new(attribute2, "key", json_string("sherpa:battery_voltage"));
	json_object_set_new(attribute2, "value", json_real(battery_voltage));
	json_array_append_new(attributes, attribute2);

	json_t* attribute3 = json_object();
	json_object_set_new(attribute3, "key", json_string("sherpa:battery_status"));
	json_object_set_new(attribute3, "value", json_string(battery_status));
	json_array_append_new(attributes, attribute3);

	json_object_set_new(newAgentNode, "attributes", attributes);
	json_object_set_new(updateBatteryNodeMsg, "node", newAgentNode);

	/* CReate message*/
	msg = encode_json_message(self, updateBatteryNodeMsg);
	/* Send the message */
	shout_message(self, msg);
	/* Wait for a reply */
	reply = wait_for_reply(self, msg, self->timeout);
	/* Print reply */
	printf("#########################################\n");
	printf("[%s] Got reply: %s \n", self->name, reply);

	/* Parse reply */
	json_error_t error;
	json_t* updateBatteryReply = json_loads(reply, 0, &error);
	json_t* querySuccessMsg = NULL;
	if(!updateBatteryReply) {
		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
	} else {
		querySuccessMsg = json_object_get(updateBatteryReply, "updateSuccess");
		char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
		printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
		free(dump);
	}
	bool querySuccess = false;

	if (querySuccessMsg) {
		//querySuccess = json_is_true(querySuccessMsg);
		querySuccess = true; //FIXME updates of same values are not yet supported
	}

	json_decref(updateBatteryNodeMsg);
	json_decref(updateBatteryReply);
	free(msg);
	free(reply);

	return querySuccess;
}

bool add_sherpa_box_status(component_t *self, sbox_status status, char* author) {

	if (self == NULL) {
		return false;
		printf("[ERROR] Communication component is not yet initialized.\n");
	}

	char* msg;
	char* reply;

	/* Get root ID to restrict search to subgraph of local SWM */
	char* root_id = 0;
	if (!get_root_node_id(self, &root_id)) {
		printf("[%s] [ERROR] Cannot get root  Id \n", self->name);
		return false;
	}

	char* batteryId = 0;
	if (!get_node_by_attribute_in_subgrapgh(self, &batteryId, "sherpa:status_type", "sherpa_box", root_id)) { // battery does not exist yet, so we will add it here

		/* Get observationGroupId */
		char* observationGroupId = 0;
		if(!get_observations_group_id(self, &observationGroupId)) {
			printf("[%s] [ERROR] Cannot get observation group Id \n", self->name);
			return false;
		}
		printf("[%s] observation Id = %s \n", self->name, observationGroupId);


		json_t *newSherpaBoxStatusNodeMsg = json_object();
		json_object_set_new(newSherpaBoxStatusNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
		json_object_set_new(newSherpaBoxStatusNodeMsg, "operation", json_string("CREATE"));
		json_object_set_new(newSherpaBoxStatusNodeMsg, "parentId", json_string(observationGroupId));
		json_t *newSBoxNode = json_object();
		json_object_set_new(newSBoxNode, "@graphtype", json_string("Node"));
		zuuid_t *uuid = zuuid_new ();
		char* sherpaBoxStatusId = zuuid_str_canonical(uuid);
		json_object_set_new(newSBoxNode, "id", json_string(sherpaBoxStatusId));

		// attributes
		json_t* attributes = json_array();
		json_t* attribute1 = json_object();
		json_object_set_new(attribute1, "key", json_string("sherpa:status_type"));
		json_object_set_new(attribute1, "value", json_string("sherpa_box"));
		json_array_append_new(attributes, attribute1);

		json_t* attribute2 = json_object();
		json_object_set_new(attribute2, "key", json_string("sherpa_box:idle"));
		json_object_set_new(attribute2, "value", json_integer(status.idle));
		json_array_append_new(attributes, attribute2);

		json_t* attribute3 = json_object();
		json_object_set_new(attribute3, "key", json_string("sherpa_box:completed"));
		json_object_set_new(attribute3, "value", json_integer(status.completed));
		json_array_append_new(attributes, attribute3);

		json_t* attribute4 = json_object();
		json_object_set_new(attribute4, "key", json_string("sherpa_box:executeId"));
		json_object_set_new(attribute4, "value", json_integer(status.executeId));
		json_array_append_new(attributes, attribute4);

		json_t* attribute5 = json_object();
		json_object_set_new(attribute5, "key", json_string("sherpa_box:commandStep"));
		json_object_set_new(attribute5, "value", json_integer(status.executeId));
		json_array_append_new(attributes, attribute5);

		json_t* attribute6 = json_object();
		json_object_set_new(attribute6, "key", json_string("sherpa_box:linActuatorPosition"));
		json_object_set_new(attribute6, "value", json_integer(status.linActuatorPosition));
		json_array_append_new(attributes, attribute6);

		json_t* attribute7 = json_object();
		json_object_set_new(attribute7, "key", json_string("sherpa_box:waspDockLeft"));
		json_object_set_new(attribute7, "value", json_boolean(status.waspDockLeft));
		json_array_append_new(attributes, attribute7);

		json_t* attribute8 = json_object();
		json_object_set_new(attribute8, "key", json_string("sherpa_box:waspDockRight"));
		json_object_set_new(attribute8, "value", json_boolean(status.waspDockRight));
		json_array_append_new(attributes, attribute8);

		json_t* attribute9 = json_object();
		json_object_set_new(attribute9, "key", json_string("sherpa_box:waspLockedLeft"));
		json_object_set_new(attribute9, "value", json_boolean(status.waspLockedLeft));
		json_array_append_new(attributes, attribute9);

		json_t* attribute10 = json_object();
		json_object_set_new(attribute10, "key", json_string("sherpa_box:waspLockedRight"));
		json_object_set_new(attribute10, "value", json_boolean(status.waspLockedRight));
		json_array_append_new(attributes, attribute10);

		json_object_set_new(newSBoxNode, "attributes", attributes);
		json_object_set_new(newSherpaBoxStatusNodeMsg, "node", newSBoxNode);

		/* CReate message*/
		msg = encode_json_message(self, newSherpaBoxStatusNodeMsg);
		/* Send the message */
		shout_message(self, msg);
		/* Wait for a reply */
		reply = wait_for_reply(self, msg, self->timeout);
		/* Print reply */
		printf("#########################################\n");
		printf("[%s] Got reply: %s \n", self->name, reply);

		/* Parse reply */
		json_error_t error;
		json_t* newSherpaBoxStatusReply = json_loads(reply, 0, &error);
		json_t* querySuccessMsg = NULL;
		if(!newSherpaBoxStatusReply) {
			printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
		} else {
			querySuccessMsg = json_object_get(newSherpaBoxStatusReply, "updateSuccess");
			char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
			printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
			free(dump);
		}
		bool querySuccess = false;

		if (querySuccessMsg) {
			querySuccess = json_is_true(querySuccessMsg);
		}

		json_decref(newSherpaBoxStatusNodeMsg);
		json_decref(newSherpaBoxStatusReply);
		free(msg);
		free(reply);
		free(sherpaBoxStatusId);
		free(uuid);

		if(!querySuccess) {
			printf("[%s] [ERROR] Can not add battery node for agent.\n", self->name);
			return false;
		}

		return true;
	}

	// if it exists already, just UPDATE the attributes

		//        batteryUpdateMsg = {
		//          "@worldmodeltype": "RSGUpdate",
		//          "operation": "UPDATE_ATTRIBUTES",
		//          "node": {
		//            "@graphtype": "Node",
		//            "id": self.battery_uuid,
		//            "attributes": [
		//                  {"key": "sensor:battery_voltage", "value": self.battery_voltage},
		//            ],
		//           },
		//        }

	json_t *updateSherpaBoxStatusNodeMsg = json_object();
	json_object_set_new(updateSherpaBoxStatusNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
	json_object_set_new(updateSherpaBoxStatusNodeMsg, "operation", json_string("UPDATE_ATTRIBUTES"));
	json_t *newSherpaBoxStatusNode = json_object();
	json_object_set_new(newSherpaBoxStatusNode, "@graphtype", json_string("Node"));
	json_object_set_new(newSherpaBoxStatusNode, "id", json_string(batteryId));

	// attributes
	json_t* attributes = json_array();
	json_t* attribute1 = json_object();
	json_object_set_new(attribute1, "key", json_string("sherpa:status_type"));
	json_object_set_new(attribute1, "value", json_string("sherpa_box"));
	json_array_append_new(attributes, attribute1);

	json_t* attribute2 = json_object();
	json_object_set_new(attribute2, "key", json_string("sherpa_box:idle"));
	json_object_set_new(attribute2, "value", json_integer(status.idle));
	json_array_append_new(attributes, attribute2);

	json_t* attribute3 = json_object();
	json_object_set_new(attribute3, "key", json_string("sherpa_box:completed"));
	json_object_set_new(attribute3, "value", json_integer(status.completed));
	json_array_append_new(attributes, attribute3);

	json_t* attribute4 = json_object();
	json_object_set_new(attribute4, "key", json_string("sherpa_box:executeId"));
	json_object_set_new(attribute4, "value", json_integer(status.executeId));
	json_array_append_new(attributes, attribute4);

	json_t* attribute5 = json_object();
	json_object_set_new(attribute5, "key", json_string("sherpa_box:commandStep"));
	json_object_set_new(attribute5, "value", json_integer(status.executeId));
	json_array_append_new(attributes, attribute5);

	json_t* attribute6 = json_object();
	json_object_set_new(attribute6, "key", json_string("sherpa_box:linActuatorPosition"));
	json_object_set_new(attribute6, "value", json_integer(status.linActuatorPosition));
	json_array_append_new(attributes, attribute6);

	json_t* attribute7 = json_object();
	json_object_set_new(attribute7, "key", json_string("sherpa_box:waspDockLeft"));
	json_object_set_new(attribute7, "value", json_boolean(status.waspDockLeft));
	json_array_append_new(attributes, attribute7);

	json_t* attribute8 = json_object();
	json_object_set_new(attribute8, "key", json_string("sherpa_box:waspDockRight"));
	json_object_set_new(attribute8, "value", json_boolean(status.waspDockRight));
	json_array_append_new(attributes, attribute8);

	json_t* attribute9 = json_object();
	json_object_set_new(attribute9, "key", json_string("sherpa_box:waspLockedLeft"));
	json_object_set_new(attribute9, "value", json_boolean(status.waspLockedLeft));
	json_array_append_new(attributes, attribute9);

	json_t* attribute10 = json_object();
	json_object_set_new(attribute10, "key", json_string("sherpa_box:waspLockedRight"));
	json_object_set_new(attribute10, "value", json_boolean(status.waspLockedRight));
	json_array_append_new(attributes, attribute10);

	json_object_set_new(newSherpaBoxStatusNode, "attributes", attributes);
	json_object_set_new(updateSherpaBoxStatusNodeMsg, "node", newSherpaBoxStatusNode);

	/* CReate message*/
	msg = encode_json_message(self, updateSherpaBoxStatusNodeMsg);
	/* Send the message */
	shout_message(self, msg);
	/* Wait for a reply */
	reply = wait_for_reply(self, msg, self->timeout);
	/* Print reply */
	printf("#########################################\n");
	printf("[%s] Got reply: %s \n", self->name, reply);

	/* Parse reply */
	json_error_t error;
	json_t* updateSherpaBoxStatusReply = json_loads(reply, 0, &error);
	json_t* querySuccessMsg = NULL;
	if(!updateSherpaBoxStatusReply) {
		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
	} else {
		querySuccessMsg = json_object_get(updateSherpaBoxStatusReply, "updateSuccess");
		char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
		printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
		free(dump);
	}
	bool querySuccess = false;

	if (querySuccessMsg) {
		//querySuccess = json_is_true(querySuccessMsg);
		querySuccess = true; //FIXME updates of same values are not yet supported
	}

	json_decref(updateSherpaBoxStatusNodeMsg);
	json_decref(updateSherpaBoxStatusReply);
	free(msg);
	free(reply);

	return querySuccess;
}

bool add_agent(component_t *self, double* transform_matrix, double utc_time_stamp_in_mili_sec, char *agent_name) {

	if (self == NULL) {
		return false;
		printf("[ERROR] Communication component is not yet initialized.\n");
	}

	char* msg;
	char* reply;

	/*
	 * Get the "root" node. It is relevant to specify a new pose.
	 */
	char* rootId = 0;
	if(!get_root_node_id(self, &rootId)) {
		printf("[%s] [ERROR] Cannot get root Id \n", self->name);
		return false;
	}
	printf("[%s] root Id = %s \n", self->name, rootId);

	/*
	 * Get the "origin" node. It is relevant to specify a new pose.
	 */
	char* originId = 0;
	if(!get_gis_origin_id(self, &originId)) {
		printf("[%s] [ERROR] Cannot get origin Id \n", self->name);
		return false;
	}
	printf("[%s] origin Id = %s \n", self->name, originId);


	char* agentId = 0;
	if (!get_node_by_attribute(self, &agentId, "sherpa:agent_name", agent_name)) { // agent does not exist yet, so we will add it here

		/* Get observationGroupId */
		char* agentsGroupId = 0;
		if(!get_node_by_attribute(self, &agentsGroupId, "name", "animals")) {
			printf("[%s] [ERROR] Cannot get agents  group Id \n", self->name);
			return false;
		}
		printf("[%s] observation Id = %s \n", self->name, agentsGroupId);

		/*
		 * Add a new agent
		 */

		// top level message
		json_t *newAgentNodeMsg = json_object();
		json_object_set_new(newAgentNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
		json_object_set_new(newAgentNodeMsg, "operation", json_string("CREATE"));
		json_object_set_new(newAgentNodeMsg, "parentId", json_string(agentsGroupId));
		json_t *newAgentNode = json_object();
		json_object_set_new(newAgentNode, "@graphtype", json_string("Group"));
		zuuid_t *uuid = zuuid_new ();
		const char* new_agentId;
		new_agentId = zuuid_str_canonical(uuid);
		json_object_set_new(newAgentNode, "id", json_string(new_agentId));

		// attributes
		json_t* attributes = json_array();
		json_t* attribute1 = json_object();
		json_object_set_new(attribute1, "key", json_string("sherpa:agent_name"));
		json_object_set_new(attribute1, "value", json_string(agent_name));
		json_array_append_new(attributes, attribute1);
		json_object_set_new(newAgentNode, "attributes", attributes);
		json_object_set_new(newAgentNodeMsg, "node", newAgentNode);

		/* CReate message*/
		msg = encode_json_message(self, newAgentNodeMsg);
		/* Send the message */
		shout_message(self, msg);
		/* Wait for a reply */
		reply = wait_for_reply(self, msg, self->timeout);
		/* Print reply */
		printf("#########################################\n");
		printf("[%s] Got reply: %s \n", self->name, reply);

		/* Parse reply */
		json_error_t error;
		json_t* newAgentReply = json_loads(reply, 0, &error);
		json_t* querySuccessMsg = NULL;
		if(!newAgentReply) {
			printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
		} else {
			querySuccessMsg = json_object_get(newAgentReply, "updateSuccess");
			char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
			printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
			free(dump);
		}
		bool querySuccess = false;

		if (querySuccessMsg) {
			querySuccess = json_is_true(querySuccessMsg);
		}

		json_decref(newAgentNodeMsg);
		json_decref(newAgentReply);
		free(msg);
		free(reply);

		if(!querySuccess) {
			printf("[%s] [ERROR] CAn nor add agent.\n", self->name);
			return false;
		}



	} // exists

	/* We will also make THIS root node a child of the agent node, so the overall structure gets more hierarchical */

//	{
//	  "@worldmodeltype": "RSGUpdate",
//	  "operation": "CREATE",
//	  "node": {
//	    "@graphtype": "Group",
//	    "id": "d0483c43-4a36-4197-be49-de829cdd66c9"
//	  },
//	  "parentId": "193db306-fd8c-4eb8-a3ab-36910665773b",
//	}
	json_t *newParentMsg = json_object();
	json_object_set_new(newParentMsg, "@worldmodeltype", json_string("RSGUpdate"));
	json_object_set_new(newParentMsg, "operation", json_string("CREATE_PARENT"));
	json_object_set_new(newParentMsg, "parentId", json_string(agentId));
	json_t *newParentNode = json_object();
	json_object_set_new(newParentNode, "@graphtype", json_string("Node"));
	json_object_set_new(newParentNode, "childId", json_string(rootId));
	json_object_set_new(newParentMsg, "node", newParentNode);

	/* Create message*/
	msg = encode_json_message(self, newParentMsg);
	/* Send the message */
	shout_message(self, msg);
	/* Wait for a reply */
	reply = wait_for_reply(self, msg, self->timeout);
	/* Print reply */
	printf("#########################################\n");
	printf("[%s] Got reply: %s \n", self->name, reply);

	/* Parse reply */
	json_error_t error;
	json_t* newParentReply = json_loads(reply, 0, &error);
	json_t* querySuccessMsg = NULL;
	if(!newParentReply) {
		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
	} else {
		querySuccessMsg = json_object_get(newParentReply, "updateSuccess");
		char* dump = json_dumps(querySuccessMsg, JSON_ENCODE_ANY);
		printf("[%s] querySuccessMsg is: %s \n", self->name, dump);
		free(dump);
	}
	bool querySuccess = false;

	if (querySuccessMsg) {
		querySuccess = json_is_true(querySuccessMsg);
	}

	json_decref(newParentMsg);
	json_decref(newParentReply);
	free(msg);
	free(reply);

	if(!querySuccess) {
		printf("[%s] [ERROR] Can not add this WMA as part of SHERPA agent. Maybe is existed already?\n", self->name);
//		return false;
	}

	char* poseId = 0;
	char poseName[512] = {0};
	snprintf(poseName, sizeof(poseName), "%s%s", agent_name, "_geopose");
	if (!get_node_by_attribute(self, &poseId, "tf:name", poseName)) { // pose does not exist yet, so we will add it here

		/*
		 * Get the "origin" node. It is relevant to specify a new pose.
		 */
		char* originId = 0;
		if(!get_gis_origin_id(self, &originId)) {
			printf("[%s] [ERROR] Cannot get origin Id \n", self->name);
			return false;
		}
		printf("[%s] origin Id = %s \n", self->name, originId);

		/*
		 * Finally add a pose ;-)
		 */
		if(!add_geopose_to_node(self, agentId, &poseId, transform_matrix, utc_time_stamp_in_mili_sec, "tf:name", poseName)) {
			printf("[%s] [ERROR] Cannot add agent pose  \n", self->name);
			return false;
		}
		printf("[%s] agent pose Id = %s \n", self->name, poseId);

	} else { // update instead
		if(!update_pose(self, transform_matrix, utc_time_stamp_in_mili_sec, agent_name)) {
			printf("[%s] [ERROR] add_agent: Cannot update pose of agent  \n", self->name);
			return false;
		}
	}

	free(agentId);
	free(poseId);

	return true;
}

bool update_pose(component_t *self, double* transform_matrix, double utc_time_stamp_in_mili_sec, char *agentName) {

	if (self == NULL) {
		return false;
		printf("[ERROR] Communication component is not yet initialized.\n");
	}
	char *msg;

	/*
	 * Get ID of pose to be updates (can be made more efficient)
	 */
	json_t *getPoseIdMsg = json_object();
	json_object_set_new(getPoseIdMsg, "@worldmodeltype", json_string("RSGQuery"));
	json_object_set_new(getPoseIdMsg, "query", json_string("GET_NODES"));
	json_t *poseIdAttribute = json_object();
	json_object_set_new(poseIdAttribute, "key", json_string("tf:name"));
    char poseName[512] = {0};
    snprintf(poseName, sizeof(poseName), "%s%s", agentName, "_geopose");
	json_object_set_new(poseIdAttribute, "value", json_string(poseName));
	json_t *attributes = json_array();
	json_array_append_new(attributes, poseIdAttribute);
	//	json_object_set(attributes, "attributes", queryAttribute);
	json_object_set_new(getPoseIdMsg, "attributes", attributes);

	/* Send message and wait for reply */
	msg = encode_json_message(self, getPoseIdMsg);
	shout_message(self, msg);
	char* reply = wait_for_reply(self, msg, self->timeout);
	printf("#########################################\n");
	printf("[%s] Got reply for agent group: %s \n", self->name, reply);

	json_decref(getPoseIdMsg);

	json_error_t error;
	json_t *poseIdReply = json_loads(reply, 0, &error);
	if(!poseIdReply) {
		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
		printf("[%s] Pose ID query not successful. \n", self->name);
		return false;
	}
	json_t* poseIdAsJSON = 0;
	json_t* poseIdArray = json_object_get(poseIdReply, "ids");
	if (poseIdArray) {
		if( json_array_size(poseIdArray) > 0 ) {
			poseIdAsJSON = json_array_get(poseIdArray, 0);
			char* dump = json_dumps(poseIdAsJSON, JSON_ENCODE_ANY);
			printf("[%s] Pose ID is: %s \n", self->name, dump);
			free(dump);
		} else {
			printf("[%s] [ERROR] Pose does not exist!\n", self->name);
			return false;
		}
	}
	free(msg);
	free(reply);

	/*
	 * Send update
	 */

    // top level message
    json_t *newTfNodeMsg = json_object();
    json_object_set_new(newTfNodeMsg, "@worldmodeltype", json_string("RSGUpdate"));
    json_object_set_new(newTfNodeMsg, "operation", json_string("UPDATE_TRANSFORM"));
//    printf("[%s] Pose ID is: %s \n", self->name, strdup( json_dumps(poseIdAsJSON, JSON_ENCODE_ANY) ));
    json_t *newTfConnection = json_object();
    json_object_set_new(newTfConnection, "@graphtype", json_string("Connection"));
    json_object_set_new(newTfConnection, "@semanticContext", json_string("Transform"));
    json_object_set(newTfConnection, "id", poseIdAsJSON);
    // Attributes

    // history
    json_t *history = json_array();
    json_t *stampedPose = json_object();
    json_t *stamp = json_object();
    json_t *pose = json_object();

    // stamp
    json_object_set_new(stamp, "@stamptype", json_string("TimeStampUTCms"));
    json_object_set_new(stamp, "stamp", json_real(utc_time_stamp_in_mili_sec));

    //pose
    json_object_set_new(pose, "type", json_string("HomogeneousMatrix44"));
    json_object_set_new(pose, "unit", json_string("latlon"));
    json_t *matrix = json_array();
    json_t *row0 = json_array();
	json_array_append_new(row0, json_real(transform_matrix[0]));
	json_array_append_new(row0, json_real(transform_matrix[4]));
	json_array_append_new(row0, json_real(transform_matrix[8]));
	json_array_append_new(row0, json_real(transform_matrix[12]));
	json_t *row1 = json_array();
	json_array_append_new(row1, json_real(transform_matrix[1]));
	json_array_append_new(row1, json_real(transform_matrix[5]));
	json_array_append_new(row1, json_real(transform_matrix[9]));
	json_array_append_new(row1, json_real(transform_matrix[13]));
	json_t *row2 = json_array();
	json_array_append_new(row2, json_real(transform_matrix[2]));
	json_array_append_new(row2, json_real(transform_matrix[6]));
	json_array_append_new(row2, json_real(transform_matrix[10]));
	json_array_append_new(row2, json_real(transform_matrix[14]));
	json_t *row3 = json_array();
	json_array_append_new(row3, json_real(transform_matrix[3]));
	json_array_append_new(row3, json_real(transform_matrix[7]));
	json_array_append_new(row3, json_real(transform_matrix[11]));
	json_array_append_new(row3, json_real(transform_matrix[15]));
    json_array_append_new(matrix, row0);
    json_array_append_new(matrix, row1);
    json_array_append_new(matrix, row2);
    json_array_append_new(matrix, row3);

    json_object_set_new(pose, "matrix", matrix);

    json_object_set_new(stampedPose, "stamp", stamp);
    json_object_set_new(stampedPose, "transform", pose);
    json_array_append_new(history, stampedPose);
    json_object_set_new(newTfConnection, "history", history);
    json_object_set_new(newTfNodeMsg, "node", newTfConnection);


    /* Send message and wait for reply */
    msg = encode_json_message(self, newTfNodeMsg);
    shout_message(self, msg);
    reply = wait_for_reply(self, msg, self->timeout);
    printf("#########################################\n");
    printf("[%s] Got reply for pose: %s \n", self->name, reply);

    /* Clean up */
    json_decref(newTfNodeMsg);
    json_decref(poseIdReply); // this has to be deleted late, since its ID is used within other queries
    //json_decref(poseIdAsJSON);
	free(msg);
    free(reply);

    return true;
}

bool get_position(component_t *self, double* xOut, double* yOut, double* zOut, double utc_time_stamp_in_mili_sec, char *agent_name) {
	double matrix[16] = { 1, 0, 0, 0,
			               0, 1, 0, 0,
			               0, 0, 1, 0,
			               0, 0, 0, 1}; // y,x,z,1 remember this is column-majo

	bool result = get_pose(self, matrix, utc_time_stamp_in_mili_sec, agent_name);

	*xOut = matrix[12];
	*yOut = matrix[13];
	*zOut = matrix[14];

	return result;
}

bool get_pose(component_t *self, double* transform_matrix, double utc_time_stamp_in_mili_sec, char *agent_name) {
	char *msg;
	json_error_t error;

	/*
	 * Get ID of agent by name
	 */
	json_t *getAgentMsg = json_object();
	json_object_set_new(getAgentMsg, "@worldmodeltype", json_string("RSGQuery"));
	json_object_set_new(getAgentMsg, "query", json_string("GET_NODES"));
	json_t *agentAttribute = json_object();
	json_object_set_new(agentAttribute, "key", json_string("sherpa:agent_name"));
	json_object_set_new(agentAttribute, "value", json_string(agent_name));
	json_t *attributes = json_array();
	json_array_append_new(attributes, agentAttribute);
	json_object_set_new(getAgentMsg, "attributes", attributes);

	/* Send message and wait for reply */
	msg = encode_json_message(self, getAgentMsg);
	shout_message(self, msg);
	char* reply = wait_for_reply(self, msg, self->timeout);
	printf("#########################################\n");
	printf("[%s] Got reply for agent group: %s \n", self->name, reply);

	free(msg);//?!?
	json_decref(getAgentMsg);

	json_t *agentIdReply = json_loads(reply, 0, &error);
//	if(!agentIdReply) {
//		printf("Error parsing JSON payload! line %d, column %d: %s\n", error.line, error.column, error.text);
//		printf("[%s] Agent ID query not successful. \n", self->name);
//		return false;
//	}
	json_t* agentIdAsJSON = 0;
	json_t* agentArray = json_object_get(agentIdReply, "ids");
	if (agentArray) {
		if( json_array_size(agentArray) > 0 ) {
			agentIdAsJSON = json_array_get(agentArray, 0);
			printf("[%s] Agent ID is: %s \n", self->name, json_dumps(json_array_get(agentArray, 0), JSON_ENCODE_ANY) );
		} else {
			printf("[%s] [ERROR] Agent does not exist. Pose query skipped.\n", self->name);
			return false;
		}
	}
	free(reply);

	/*
	 * Get origin ID
	 */
	json_t *getOriginMsg = json_object();
	json_object_set_new(getOriginMsg, "@worldmodeltype", json_string("RSGQuery"));
	json_object_set_new(getOriginMsg, "query", json_string("GET_NODES"));
	json_t * originAttribute = json_object();
	json_object_set_new(originAttribute, "key", json_string("gis:origin"));
	json_object_set_new(originAttribute, "value", json_string("wgs84"));
	attributes = json_array();
	json_array_append_new(attributes, originAttribute);
	json_object_set_new(getOriginMsg, "attributes", attributes);

	/* Send message and wait for reply */
    msg = encode_json_message(self, getOriginMsg);
    shout_message(self, msg);
    reply = wait_for_reply(self, msg, self->timeout);
    printf("#########################################\n");
    printf("[%s] Got reply: %s \n", self->name, reply);

    json_decref(getOriginMsg);

    /* Parse reply */
    json_t *originIdReply = json_loads(reply, 0, &error);
    free(msg);
    free(reply);
    json_t* originIdAsJSON = 0;
    json_t* originArray = json_object_get(originIdReply, "ids");
    if (originArray) {
    	printf("[%s] result array found. \n", self->name);
    	if( json_array_size(originArray) > 0 ) {
    		originIdAsJSON = json_array_get(originArray, 0);
        	printf("[%s] Origin ID is: %s \n", self->name, json_dumps(originIdAsJSON, JSON_ENCODE_ANY));
    	} else {
			printf("[%s] [ERROR] Origin does not exist. Pose query skipped.\n", self->name);
			return false;
		}
    }
	/*
	 * Get pose at time utc_time_stamp_in_mili_sec
	 */
//    {
//      "@worldmodeltype": "RSGQuery",
//      "query": "GET_TRANSFORM",
//      "id": "3304e4a0-44d4-4fc8-8834-b0b03b418d5b",
//      "idReferenceNode": "e379121f-06c6-4e21-ae9d-ae78ec1986a1",
//      "timeStamp": {
//        "@stamptype": "TimeStampDate",
//        "stamp": "2015-11-09T16:16:44Z"
//      }
//    }
	json_t *getTransformMsg = json_object();
	json_object_set_new(getTransformMsg, "@worldmodeltype", json_string("RSGQuery"));
	json_object_set_new(getTransformMsg, "query", json_string("GET_TRANSFORM"));
	json_object_set(getTransformMsg, "id", agentIdAsJSON);
	json_object_set(getTransformMsg, "idReferenceNode", originIdAsJSON);
	// stamp
	json_t *stamp = json_object();
	json_object_set_new(stamp, "@stamptype", json_string("TimeStampUTCms"));
	json_object_set_new(stamp, "stamp", json_real(utc_time_stamp_in_mili_sec));
	json_object_set_new(getTransformMsg, "timeStamp", stamp);

	/* Send message and wait for reply */
    msg = encode_json_message(self, getTransformMsg);
    shout_message(self, msg);
    reply = wait_for_reply(self, msg, self->timeout);
    printf("#########################################\n");
    printf("[%s] Got reply: %s \n", self->name, reply);

	/*
	 * Parse result
	 */

    /* Parse reply */
    json_t *transformReply = json_loads(reply, 0, &error);
    json_t* transform = json_object_get(transformReply, "transform");
    free(msg);
    free(reply);
    if(transform) {
    	json_t* matrix = json_object_get(transform, "matrix");
    	transform_matrix[0]  = json_real_value(json_array_get(json_array_get(matrix, 0), 0));
    	transform_matrix[4]  = json_real_value(json_array_get(json_array_get(matrix, 0), 1));
    	transform_matrix[8]  = json_real_value(json_array_get(json_array_get(matrix, 0), 2));
    	transform_matrix[12] = json_real_value(json_array_get(json_array_get(matrix, 0), 3));

    	transform_matrix[1]  = json_real_value(json_array_get(json_array_get(matrix, 1), 0));
    	transform_matrix[5]  = json_real_value(json_array_get(json_array_get(matrix, 1), 1));
    	transform_matrix[9]  = json_real_value(json_array_get(json_array_get(matrix, 1), 2));
    	transform_matrix[13] = json_real_value(json_array_get(json_array_get(matrix, 1), 3));

    	transform_matrix[2]  = json_real_value(json_array_get(json_array_get(matrix, 2), 0));
    	transform_matrix[6]  = json_real_value(json_array_get(json_array_get(matrix, 2), 1));
    	transform_matrix[10] = json_real_value(json_array_get(json_array_get(matrix, 2), 2));
    	transform_matrix[14] = json_real_value(json_array_get(json_array_get(matrix, 2), 3));

    	transform_matrix[3]  = json_real_value(json_array_get(json_array_get(matrix, 3), 0));
    	transform_matrix[7]  = json_real_value(json_array_get(json_array_get(matrix, 3), 1));
    	transform_matrix[11] = json_real_value(json_array_get(json_array_get(matrix, 3), 2));
    	transform_matrix[15] = json_real_value(json_array_get(json_array_get(matrix, 3), 3));

    } else {
    	return false;
    }

    json_decref(getTransformMsg);
    json_decref(agentIdReply); // this has to be deleted late, since its ID is used within other queries
    json_decref(originIdReply);
    json_decref(transformReply);

	return true;
}

