#ifndef __EXEC_CMDS__
#define __EXEC_CMDS__

typedef int (*cmd_handler)(int, const char**);

struct cmd_struct
{
	char *cmd;
	cmd_handler handler;
};

int cmd_mktag(int argc, const char **);

#endif /* __EXEC_CMDS__ */
