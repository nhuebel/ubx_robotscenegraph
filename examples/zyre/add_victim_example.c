/**
 * Example on how the send RSG-JSON messages via zyre.
 * It uses the swmzyre library as helper.
 */

#include <zyre.h>
#include <jansson.h>
#include <uuid/uuid.h>
#include <string.h>

#include "swmzyre.h"

int main(int argc, char *argv[]) {

	char agent_name[] = "fw0"; // or wasp1, operator0, ... donkey0, sherpa_box0. Please use the same as SWM_AGENT_NAME environment variable.

	/* Load configuration file for communication setup */
	char config_folder[255] = { SWM_ZYRE_CONFIG_DIR };
	char config_name[] = "swm_zyre_config.json";
	char config_file[512] = {0};
	snprintf(config_file, sizeof(config_file), "%s/%s", config_folder, config_name);

    if (argc == 2) { // override default config
    	snprintf(config_file, sizeof(config_file), "%s", argv[1]);
    }
    
    json_t * config = load_config_file(config_file);//"swm_zyre_config.json");
    if (config == NULL) {
      return -1;
    }

    /* Spawn new communication component */
    component_t *self = new_component(config);
    if (self == NULL) {
    	return -1;
    }
    printf("[%s] component initialized!\n", self->name);
    char *msg;

	/* Input variables */
	double x = 979875;
	double y = 48704;
	double z = 405;
	double utcTimeInMiliSec = 0.0;

	int i;
	struct timeval tp;

	for (i = 0; i < 2; ++i) {
		printf("###################### VICTIM #########################\n");
		add_victim(self, x,y,z,utcTimeInMiliSec, agent_name);
	}

	printf("###################### AGENT #########################\n");
	add_agent(self,  x,y,z,utcTimeInMiliSec, agent_name); //TODO rotation/transform as 4x4 column-major matrix

	for (i = 0; i < 30; ++i) {
			printf("######################  POSE  #########################\n");
			gettimeofday(&tp, NULL);
			utcTimeInMiliSec = tp.tv_sec * 1000 + tp.tv_usec / 1000; //get current timestamp in milliseconds
			update_pose(self, x,y,z+i,utcTimeInMiliSec+i, agent_name);
			usleep(100/*[ms]*/ * 1000);
	}

	printf("######################  GET POSITION  #########################\n");
	x = 0;
	y = 0;
	z = 0;
	gettimeofday(&tp, NULL);
	utcTimeInMiliSec = tp.tv_sec * 1000 + tp.tv_usec / 1000; //get current timestamp in milliseconds
	get_position(self, &x, &y, &z, utcTimeInMiliSec, agent_name);
	printf ("Latest position of hawk = (%f,%f,%f)\n", x,y,z);


	printf("######################  DONE  #########################\n");
    /* Clean up */
    destroy_component(&self);
    printf ("SHUTDOWN\n");

	return 0;

}

