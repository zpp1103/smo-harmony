#pragma once
int pthread_set_qos_class_self_np(int cls, int rel);
typedef int qos_class_t;
#define QOS_CLASS_UNSPECIFIED -1
#define QOS_CLASS_USER_INTERACTIVE 0
#define QOS_CLASS_USER_INITIATED 1
#define QOS_CLASS_DEFAULT 2
#define QOS_CLASS_UTILITY 3
