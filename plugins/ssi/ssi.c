#include <uwsgi.h>

#define UWSGI_SSI_MAX_ARGS 8

/*

	uWSGI server side includes implementation

*/


struct uwsgi_ssi_arg {
	char *key;
	size_t key_len;
	char *value;
	size_t val_len;
};

struct uwsgi_ssi_cmd {
	char *name;
	size_t name_len;
	struct uwsgi_buffer *(*func)(struct wsgi_request *, struct uwsgi_ssi_arg *, int);
	struct uwsgi_ssi_cmd *next;
};

struct uwsgi_ssi_cmd *uwsgi_ssi_commands = NULL;

static struct uwsgi_ssi_cmd* uwsgi_ssi_get_cmd(char *name, size_t name_len) {
	struct uwsgi_ssi_cmd *usc = uwsgi_ssi_commands;
	while(usc) {
		if (!uwsgi_strncmp(usc->name, usc->name_len, name, name_len)) {
			return usc;
		}
		usc = usc->next;
	}
	return NULL;
}

static struct uwsgi_ssi_cmd *uwsgi_register_ssi_command(char *name, struct uwsgi_buffer *(*func)(struct wsgi_request *, struct uwsgi_ssi_arg *, int)) {
	struct uwsgi_ssi_cmd *old_usc = NULL,*usc = uwsgi_ssi_commands;
        while(usc) {
                if (!strcmp(usc->name, name)) {
                        return usc;
                }
                old_usc = usc;
                usc = usc->next;
        }

        usc = uwsgi_calloc(sizeof(struct uwsgi_ssi_cmd));
        usc->name = name;
        usc->name_len = strlen(name);
        usc->func = func;

        if (old_usc) {
                old_usc->next = usc;
        }
        else {
                uwsgi_ssi_commands = usc;
        }

        return usc;
}

static void uwsgi_ssi_parse_args(struct wsgi_request *wsgi_req, char *buf, size_t len, struct uwsgi_ssi_arg *argv, int *argc) {
	// status [0]null/= [1]" [2]" [3]\s
	size_t i;
	uint8_t status = 0;
	char *key = buf;
	size_t key_len = 0;
	char *value = NULL;
	size_t val_len = 0;
	for(i=0;i<len;i++) {
		switch(status) {
			case 0:
				if (buf[i] == '=') {
					status = 1;
				}
				else {
					key_len++;
				}
				break;
			case 1:
				if (buf[i] == '"') {
					status = 2;
				}
				else {
					return;
				}
				break;
			case 2:
				if (buf[i] == '"') {
					status = 3;	
					argv[*argc].key = key; argv[*argc].key_len = key_len;
					argv[*argc].value = value; argv[*argc].val_len = val_len;
					*argc = *argc+1;
					if (*argc >= UWSGI_SSI_MAX_ARGS) {
						return;
					}
					key = NULL;
					key_len = 0;
					value = NULL;
					val_len = 0;
				}
				else {
					if (!value) {
						value = buf + i;
					}
					val_len++;
				}
				break;
			case 3:
				if (!isspace((int)buf[i])) {
					key = buf + i;
					key_len = 1;
					status = 0;
				}
				break;
			default:
				return;
		}
	}
}

static void uwsgi_ssi_parse_command(struct wsgi_request *wsgi_req, char *buf, size_t len) {
	// first remove white spaces from the begin and the end
	char *cmd = buf;
	size_t cmd_len = len;
        size_t i;
        for(i=0;i<len;i++) {
                if (isspace((int)buf[i])) {
                        cmd++;
                        cmd_len--;
                }
                else {
                        break;
                }
        }

        // then rstrip (skipping the first char...)
        for(i=(len-1);i>0;i--) {
                if (isspace((int)buf[i])) {
                        cmd_len--;
                }
                else {
                        break;
                }
        }

	uwsgi_log("CMD = |%.*s|\n", cmd_len, cmd);

	// now get the command
	char *ssi_cmd = cmd;
	size_t ssi_cmd_len = 0;
	int found = 0;

	for(i=0;i<cmd_len;i++) {
		if (isspace(cmd[i])) {
			found = 1;
			break;
		}
		ssi_cmd_len++;
	}

	if (!found) {
		return;
	}

	uwsgi_log("SSI cmd = ^%.*s^\n", ssi_cmd_len, ssi_cmd);

	struct uwsgi_ssi_cmd *usc = uwsgi_ssi_get_cmd(ssi_cmd, ssi_cmd_len);
	if (!usc) return ;

	// now split the args
	struct uwsgi_ssi_arg argv[UWSGI_SSI_MAX_ARGS];
	int argc = 0;

	char *cmd_args = cmd + ssi_cmd_len + 1;
	size_t cmd_args_len = cmd_len - (ssi_cmd_len + 1);

	for(i=(ssi_cmd_len + 1);i<cmd_len;i++) {
		if (isspace((int)cmd[i])) {
			cmd_args++;
			cmd_args_len--;
		}
		else {
			break;
		}
	}

	uwsgi_log("SSI args = #%.*s#\n", cmd_args_len, cmd_args);

	uwsgi_ssi_parse_args(wsgi_req, cmd_args, cmd_args_len, argv, &argc);

	struct uwsgi_buffer *ub = usc->func(wsgi_req, argv, argc);
	if (ub) {
		uwsgi_log("RETURN = %.*s\n", ub->pos, ub->buf);
		uwsgi_buffer_destroy(ub);
	}
	
}

static void uwsgi_ssi_parse(struct wsgi_request *wsgi_req, char *buf, size_t len) {
	size_t i;
	uint8_t status = 0;
	char *cmd = NULL;
	size_t cmd_len = 0;
	// parsing status 0[null] 1[<] 2[!] 3[-] 4[-] 5[#/-] 6[-] 7[>]
        // on status 6-7-8 the reset action come back to 5 instead of 0 
	for(i=0;i<len;i++) {
		switch(status) {
			case 0:
				if (buf[i] == '<') {
					status = 1;
				}
				break;
			case 1:
				status = 0;
				if (buf[i] == '!') {
					status = 2;
				}
				break;
			case 2:
				status = 0;
				if (buf[i] == '-') {
					status = 3;
				}
				break;
			case 3:
				status = 0;
				if (buf[i] == '-') {
					status = 4;
				}
				break;
			case 4:
				status = 0;
				if (buf[i] == '#') {
					status = 5;
				}
				break;
			case 5:
				if (buf[i] == '-') {
					status = 6;
					break;
				}
				if (!cmd) {
					cmd = buf+i;
				}
				cmd_len++;
				break;
			case 6:
				status = 5;
				if (buf[i] == '-') {
					status = 7;
				}
				else {
					cmd_len+=2;
				}
				break;
			case 7:
				status = 5;
				if (buf[i] == '>') {
					status = 0;
					uwsgi_ssi_parse_command(wsgi_req, cmd, cmd_len);
					cmd = NULL;
					cmd_len = 0;	
                                }
                                else {
                                        cmd_len+=3;
                                }
				break;
			default:
				return;
		}
	}
}

static int uwsgi_ssi_request(struct wsgi_request *wsgi_req) {

	if (uwsgi_parse_vars(wsgi_req)) {
                return -1;
        }

	char buf[32768];
	int fd = open("foo.shtml", O_RDONLY);

	ssize_t len = read(fd, buf, 32768);
	uwsgi_log("LEN = %d\n", len);

	uwsgi_ssi_parse(wsgi_req, buf, len);

	close(fd);
	return UWSGI_OK;
}

static char *uwsgi_ssi_get_arg(struct uwsgi_ssi_arg *argv, int argc, char *key, size_t key_len, size_t *val_len) {
	int i;
	for(i=0;i<argc;i++) {
		struct uwsgi_ssi_arg *arg = &argv[i];
		if (!uwsgi_strncmp(arg->key, arg->key_len, key, key_len)) {
			*val_len = arg->val_len;	
			return arg->value;
		}
	}

	return NULL;
}

// echo command
static struct uwsgi_buffer *ssi_cmd_echo(struct wsgi_request *wsgi_req, struct uwsgi_ssi_arg *argv, int argc) {
	uwsgi_log("CALLING ECHo !!!\n");
	size_t var_len = 0;
	char *var = uwsgi_ssi_get_arg(argv, argc, "var", 3, &var_len);

	if (!var || var_len == 0) return NULL;

	uwsgi_log("CHECKING VAR = %.*s\n", var_len, var);

	uint16_t rlen = 0;
	char *value = uwsgi_get_var(wsgi_req, var, var_len, &rlen);
	if (!value) return NULL;
	if (rlen == 0) return NULL;

	struct uwsgi_buffer *ub = uwsgi_buffer_new(rlen);
	if (uwsgi_buffer_append(ub, value, rlen)) {
		uwsgi_buffer_destroy(ub);
		return NULL;
	}

	uwsgi_log("returning BUF\n");
	return ub;
};

static int uwsgi_ssi_init() {
	uwsgi_register_ssi_command("echo", ssi_cmd_echo);
	return 0;
}

struct uwsgi_plugin ssi_plugin = {
	.name = "ssi",
	.modifier1 = 19,
	.init = uwsgi_ssi_init,
	.request = uwsgi_ssi_request,
};