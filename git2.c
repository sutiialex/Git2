#include "exec_cmd.h"
#include <stdio.h>
#include <string.h>

struct cmd_struct commands[] = {
	{"mktag",	cmd_mktag}
};

void print_usage()
{
	printf("Git2 available commands:\n");
	printf("	mktag\n");
}

cmd_handler lookup_handler(char *cmd)
{
	int i;
	for (i = 0; i < sizeof(commands); i++)
		if (!strcmp(commands[i].cmd, cmd))
			return commands[i].handler;
	return NULL;
}

int main(int argc, char **argv)
{
	cmd_handler handler;

	if (argc < 2)
	{
		print_usage();
		return -1;
	}

	if ((handler = lookup_handler(argv[1])) == NULL)
	{
		printf("Unknown commad '%s'.\n", argv[1]);
		print_usage();
		return -1;
	}

	return handler(argc-2, (const char**)(argv+2));
}
