/*
 * rsg_scene_setup microblx function block (autogenerated, don't edit)
 */

#include <ubx.h>

/* includes types and type metadata */

ubx_type_t types[] = {
        { NULL },
};

/* block meta information */
char rsg_scene_setup_meta[] =
        " { doc='',"
        "   real-time=true,"
        "}";

/* declaration of block configuration */
ubx_config_t rsg_scene_setup_config[] = {
        { .name="wm_handle", .type_name = "struct rsg_wm_handle", .doc="Handle to the world wodel instance. This parameter is mandatory." },
        { .name="log_level", .type_name = "int", .doc="Set the log level: LOGDEBUG = 0, INFO = 1, WARNING = 2, LOGERROR = 3, FATAL = 4" },
        { NULL },
};

/* declaration port block ports */
ubx_port_t rsg_scene_setup_ports[] = {
        { .name="rsg_out", .out_type_name="unsigned char", .out_data_len=1, .doc="HDF5 based byte stream for updates on RSG based world model."  },
        { NULL },
};

/* declare a struct port_cache */
struct rsg_scene_setup_port_cache {
        ubx_port_t* rsg_out;
};

/* declare a helper function to update the port cache this is necessary
 * because the port ptrs can change if ports are dynamically added or
 * removed. This function should hence be called after all
 * initialization is done, i.e. typically in 'start'
 */
static void update_port_cache(ubx_block_t *b, struct rsg_scene_setup_port_cache *pc)
{
        pc->rsg_out = ubx_port_get(b, "rsg_out");
}


/* for each port type, declare convenience functions to read/write from ports */
//def_write_fun(write_rsg_out, unsigned char)

/* block operation forward declarations */
int rsg_scene_setup_init(ubx_block_t *b);
int rsg_scene_setup_start(ubx_block_t *b);
void rsg_scene_setup_stop(ubx_block_t *b);
void rsg_scene_setup_cleanup(ubx_block_t *b);
void rsg_scene_setup_step(ubx_block_t *b);


/* put everything together */
ubx_block_t rsg_scene_setup_block = {
        .name = "rsg_scene_setup",
        .type = BLOCK_TYPE_COMPUTATION,
        .meta_data = rsg_scene_setup_meta,
        .configs = rsg_scene_setup_config,
        .ports = rsg_scene_setup_ports,

        /* ops */
        .init = rsg_scene_setup_init,
        .start = rsg_scene_setup_start,
        .stop = rsg_scene_setup_stop,
        .cleanup = rsg_scene_setup_cleanup,
        .step = rsg_scene_setup_step,
};


/* rsg_scene_setup module init and cleanup functions */
int rsg_scene_setup_mod_init(ubx_node_info_t* ni)
{
        DBG(" ");
        int ret = -1;
        ubx_type_t *tptr;

        for(tptr=types; tptr->name!=NULL; tptr++) {
                if(ubx_type_register(ni, tptr) != 0) {
                        goto out;
                }
        }

        if(ubx_block_register(ni, &rsg_scene_setup_block) != 0)
                goto out;

        ret=0;
out:
        return ret;
}

void rsg_scene_setup_mod_cleanup(ubx_node_info_t *ni)
{
        DBG(" ");
        const ubx_type_t *tptr;

        for(tptr=types; tptr->name!=NULL; tptr++)
                ubx_type_unregister(ni, tptr->name);

        ubx_block_unregister(ni, "rsg_scene_setup");
}

/* declare module init and cleanup functions, so that the ubx core can
 * find these when the module is loaded/unloaded */
UBX_MODULE_INIT(rsg_scene_setup_mod_init)
UBX_MODULE_CLEANUP(rsg_scene_setup_mod_cleanup)
