#ifndef OSC_CONTROLLER_H
#define OSC_CONTROLLER_H

struct controller;
struct rt;

int osc_init(struct controller *c, struct rt *rt, const char *port);

#endif
