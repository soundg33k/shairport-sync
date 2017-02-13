#include "shairport_lcm.h"
#include "shairport_lcm_t.h"
#include "shairport_lcm_t.c"

static lcm_t * lcm = NULL;

void shairport_lcm_init(){
	lcm = lcm_create(NULL);
}

void shairport_lcm_shutdown(){
	if(lcm)
	{
		lcm_destroy(lcm);
		lcm = NULL;
	}
}

void shairport_lcm_volume(float volume){
	if(lcm)
	{
		shairport_lcm_t message;
		message.message = "volume";
		message.value = volume;
		shairport_lcm_t_publish(lcm,"DEFAULT", &message);
	}
}

void shairport_lcm_start(){
	if(lcm)
        {
		shairport_lcm_t message;
		message.message = "play";
		message.value = 0;
		shairport_lcm_t_publish(lcm,"DEFAULT", &message);
        }
}

void shairport_lcm_stop(){
	if(lcm)
        {
		shairport_lcm_t message;
		message.message = "stop";
		message.value = 0;
		shairport_lcm_t_publish(lcm,"DEFAULT", &message);
        }
}
