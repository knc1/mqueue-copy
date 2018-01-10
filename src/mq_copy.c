/*
 * Copyright (c) 2017, 2018 M. S. Zick
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * A copy of the license is included with this work and in addition;
 * You may obtain a copy of the License at:
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <libgen.h>     /* POSIX version of basename */
#include <string.h>
#include <stdlib.h>
#include <getopt.h>     /* getopt_long, getopt_long_only */
#include <fcntl.h>      /* For O_* constants */
#include <sys/stat.h>   /* For mode constants */
#include <mqueue.h>
#include <errno.h>
#include <time.h>

/* Provided by getopt.h
 * char *optarg;    pointer to argument following the current option
 * int   optind;    index to next argument to be processed
 * int   opterr;    set to 0 disables error messages to stderr
 * int   optopt;    option character if not recognized
 */

/* AT from: http://www.decompile.com/cpp/faq/file_and_line_error_string.htm */
#define STRY(x) #x
#define NTOS(x) STRY(x)
#define AT __FILE__":"NTOS(__LINE__)
#define STRBUF 80
#define STRSTR 78
/* But these two macros are my very own.
 * If the only output is a string, terminate the string with "%s" and pass
 * an argument of a blank string "".
 */
#define LocErr(format, args...) do { \
                                    snprintf(Estr, STRSTR, format, args); \
                                    fprintf(stderr, "Warning at %s> %s\n", AT, Estr); \
                                    } while (0)
#define SysErr(format, args...) do { \
                                    snprintf(Estr, STRSTR, format, args); \
                                    fprintf(stderr, "Error at %s> %s, System result: %s\n", AT, Estr, \
                                    strerror(errno)); \
                                    } while (0)
char Estr[STRBUF];
void StrErr(const char *location, const char *msg) {
    fprintf(stderr, "Info at %s> %s\n", location, msg);
}

/* Supporting */
int ts2abs(struct timespec *ts, int tsec);  /* +1/10s second to current time */
int copy_out(mqd_t mqd, int mlen, int tblk, int wtime);
int copy_in(mqd_t mqd, int mlen, int pri, int tblk, int wtime);

/* mq_copy
 * 
 * One of the following options may be specified:
 * -r --ro     : Queue is used read-only
 * -w --wo     : Queue is used write-only
 * Without a specification, open will default to r/w.
 * 
 * Default operation is blocking (receive blocks on empty, send blocks on full)
 * -n --nb, --no-block : Fail with EAGAIN instead of blocking
 * 
 * Default action: Open an existing message queue, RW, blocking.
 * 
 * -c --cr, --create       : Open existing or Create and open if does not exist
 * User and group are set to the effective user and group of the caller.
 * 
 * -m --md, --mode <perm>  : Set user and group permissions otherwise: 0660
 * <perm> must be either three or four octal digits (no syms, sorry about that)
 * 
 * -a --at, --attributes <at-list> : Set attributes, else use program defaults
 * Where <at-list> is one or more, comma seperated, name=value pairs or feature.
 * 
 * <at-list> example: -a maxmsgs=10,msgsize=256,noblock
 *      maxmsgs : The maximum number of messages the queue will hold.
 *      msgsize : The maximum byte length of any single message.
 *      noblock : Disable the default blocking behavior.
 * 
 * -x --ex, --exclusive    : If name exists, fail with EEXIST
 * Unlike open(2), --exclusive is only used in combination with --create
 * 
 * -u --ul, --unlink    : unlink when closing
 * 
 * The following symbolic constants for mode are the same as for open(2) :
 * (execute permission is ignored)
 * S_IRWXU  00700 user (file owner) has read, write, and execute permission
 * S_IRUSR  00400 user has read permission
 * S_IWUSR  00200 user has write permission
 * S_IRWXG  00070 group has read, write, and execute permission
 * S_IRGRP  00040 group has read permission
 * S_IWGRP  00020 group has write permission
 * S_IRWXO  00007 others have read, write, and execute permission
 * S_IROTH  00004 others have read permission
 * S_IWOTH  00002 others have write permission
 * The current umask is applied to the specified options.
 */
int main(int argc, char *argv[]) {
    int opt, indx, rwf, oflag, cflag, ul_flag = 0;
    int ci_flag, co_flag, wtime = 0, tblk = 0;
    int slen, nmode, nret, ival, do_die = 0;
    int pri = 15; /* a reasonable default (some systems priority is 0..31) */
    char ch, *subopts, *val, *saved, *fpath, *bname, *qname = NULL;
    int mlen;
    mqd_t mqd = (mqd_t)0;
    struct timespec abst;

    /* only used when calling the 'create' version of mq_copy */
    mode_t perms = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP; /* Default: 0660 */
    struct mq_attr attrs, qattrs;

    fpath = strdup(argv[0]);
    bname = basename(fpath);

    /* mq_copy queue_name <- absolute minimum */
    if (argc < 2) {
        LocErr("\'%s\' requires at least one argument", bname);
        exit(EXIT_FAILURE);
    }

    /* Place some reasonable defaults */
    attrs.mq_flags = 0;         /* flags: 0 (Blocking) or O_NONBLOCK */
    /* At least glibc-2.23 clobbers mq_flags, so will set them ourselves */
    attrs.mq_maxmsg = 10;       /* Maximum queue length (messages) */
    attrs.mq_msgsize = 1024;    /* Maximum message size (bytes) */
    attrs.mq_curmsgs = 0;       /* Current queue length (messages) */

    opterr = 0;                 /* Invent our own error messages */

    /* Sub-option parser */
    enum {
        MAX_MSGS = 0,
        MSG_SIZE,
        SET_NONBLOCK
    };

    const char *attr_opts[] = {
        [MAX_MSGS] = "maxmsgs",
        [MSG_SIZE] = "msgsize",
        [SET_NONBLOCK] = "noblock",
        NULL
    };

    static struct option loptions[] = {
        {"ro",          no_argument,        NULL, 'r'},
        {"wo",          no_argument,        NULL, 'w'}, /* w is reserved, so non-POSIX */
        {"nb",          no_argument,        NULL, 'n'},
        {"no-block",    no_argument,        NULL, 'n'},
        {"cr",          no_argument,        NULL, 'c'},
        {"create",      no_argument,        NULL, 'c'},
        {"md",          required_argument,  NULL, 'm'},
        {"mode",        required_argument,  NULL, 'm'},
        {"at",          required_argument,  NULL, 'a'},
        {"attributes",  required_argument,  NULL, 'a'},
        {"ex",          no_argument,        NULL, 'x'},
        {"exclusive",   no_argument,        NULL, 'x'},
        {"pri",         required_argument,  NULL, 'p'},
        {"priority",    required_argument,  NULL, 'p'},
        {"in",          no_argument,        NULL, 'i'}, /* copy stdin -> queue */
        {"ci",          no_argument,        NULL, 'i'},
        {"out",         no_argument,        NULL, 'o'}, /* copy queue -> stdout */
        {"co",          no_argument,        NULL, 'o'},
        {"time",        required_argument,  NULL, 't'}, /* max wait time to block */
        {"ul",          no_argument,        NULL, 'u'},
        {"unlink",      no_argument,        NULL, 'u'}, /* unlink at close */
        {0,0,0,0}
    };

    rwf = 0;
    oflag = 0;
    cflag = 0;
    ci_flag = 0;
    co_flag = 0;
    indx = 0;       /* getopt_long can fail if not initialized */
    while ((opt = getopt_long(argc, argv, "rwncm:a:xiot:pu", loptions, &indx)) != -1) {
        switch (opt) {
            case 'r' : rwf += 1;
                break;
            case 'w' : rwf += 2;
                break;
            case 'n' : /* oflag |= O_NONBLOCK; */
                break;
            case 'c' : oflag |= O_CREAT; cflag = 1;
                break;
            case 'i' : ci_flag = 1; co_flag = 0;    /* last one on  */
                break;
            case 'o' : co_flag = 1; ci_flag = 0;    /* command wins */
                break;
            case 'u' : ul_flag = 1;
                break;
            case 'p' :
                nret = sscanf(optarg, "%d%c", &pri, &ch);
                if (nret != 1 || pri < 0 || pri > 31) {
                    LocErr("Option -p with bad argument %s", optarg);
                    do_die = 1;
                }
                break;
            case 't' :
                nret = sscanf(optarg, "%d%c", &wtime, &ch);
                if (nret != 1 || wtime < 0) {   /* positive time only */
                    LocErr("Option -t with bad argument %s", optarg);
                    do_die = 1;
                } else tblk = 1;
                break;
            case 'm' :
                slen = strlen(optarg);
                if (slen == 3 || slen == 4 ) {
                    nret = sscanf(optarg, "%o%c", &nmode, &ch);
                    if ( nret != 1 || nmode < 128) { /* owner:write is minimum */
                        LocErr("Option -m with bad argument %s", optarg);
                        do_die = 1;
                    } else perms = nmode; /* good conversion */ 
                } else { /* bad length */
                    LocErr("Option -m argument %s bad length %d", optarg, slen);
                    do_die = 1;
                }
                break;
            case 'a' :
                subopts = optarg;
                while (*subopts != '\0') {
                    saved = subopts;
                    switch (getsubopt(&subopts, (char **)attr_opts, &val)) {
                        case MAX_MSGS:
                            if (val == NULL) {
                                do_die = 1;
                            } else {
                                nret = sscanf(val, "%d%c", &ival, &ch);
                                if (nret != 1) {
                                    LocErr("Bad arguement %s for maximum messages", val);
                                    do_die = 1;
                                } else attrs.mq_maxmsg = ival;
                            }
                            break;
                        case MSG_SIZE:
                            if (val == NULL) {
                                do_die = 1;
                            } else {
                                nret = sscanf(val, "%d%c", &ival, &ch);
                                if (nret != 1) {
                                    LocErr("Bad arguement %s for maximum message size", val);
                                    do_die = 1;
                                } else attrs.mq_msgsize = ival;
                            }
                            break;
                        case SET_NONBLOCK:
                          /*  attrs.mq_flags |= O_NONBLOCK; */
                          /*  oflag |= O_NONBLOCK; */
                            break;
                        default:
                            LocErr("Option -a, unknown suboption %s", saved);
                            do_die = 1;
                    }
                }
                break;
            case 'x' : 
                oflag |= O_EXCL; oflag |= O_CREAT; cflag = 1;
                attrs.mq_flags |= O_EXCL; attrs.mq_flags |= O_CREAT;
                break;
            case ':' :
                LocErr("Option %s, argument missing", optarg);
                do_die = 1;
                break;
            case '?' :
                LocErr("Unrecognized option argument %s", optarg);
                do_die = 1;
                break;
            default: 
                LocErr("Unrecognized option, option %c", (char)opt);
                do_die = 1;
        }
    }

    /* No reason to continue if parse errors */
    if (do_die) {
        LocErr("Parse error exit%s", "");
        exit(EXIT_FAILURE);
    }

    /* The queue name should be last after getopt_long finshes */
    if (optind <= argc - 1) {    /* An unprocessed string remains */
        qname = strdup(argv[argc - 1]);
        if (qname == NULL) {
            LocErr("Queue name is null%s", "");
            do_die = 1;
        } else {
            slen = strlen(qname);
            if (slen <= 0) {
                LocErr("Bad string lenght %d", slen);
                do_die = 1;
            }
        }
    } else {
        LocErr("Bad optind%s", "");
        do_die = 1;
    }

    /* Must have a usable queue name to continue */
    if (do_die) {
        LocErr("Queue name \'%s\', error exit", qname);
        exit(EXIT_FAILURE);
    }

    switch (rwf) {
        case 0 :
            rwf = O_RDWR;      /* Make default queue type read/write */
            attrs.mq_flags |= O_RDWR;
            break;
        case 1 :
            rwf = O_RDONLY;
            attrs.mq_flags |= O_RDONLY;
            break;
        case 2 :
            rwf = O_WRONLY;
            attrs.mq_flags |= O_WRONLY;
            break;
        default:
            rwf = O_RDWR;      /* User specified both */
            attrs.mq_flags |= O_RDWR;
        }
    oflag |= rwf;

    /* Track if we need to clean up something before failure exit */
    if (cflag) {    /* Create option to: "open" */
        mqd = mq_open(qname, oflag, perms, &attrs);
        if (mqd == (mqd_t)-1) {
            SysErr("Queue create failed%s", "");
            do_die = 1;
        }
    } else {          /* Only "open" - queue must exist */
        mqd = mq_open(qname, oflag);
        if (mqd == (mqd_t)-1) {
            SysErr("Queue open failed%s", "");
            do_die = 1;
        }
    }

    /* Use actual queue msgsize for read buffer length */
    if (mq_getattr(mqd, &qattrs) == -1) {
        SysErr("Get attributes of descriptor: %d failed", mqd);
        do_die = 1;
    }
    mlen = qattrs.mq_msgsize;

    if (do_die == 0) {
        if (co_flag) {
            do_die = copy_out(mqd, mlen, tblk, wtime);
        }
        else if (ci_flag) {
            do_die = copy_in(mqd, mlen, pri, tblk, wtime);
        }
        else {
            LocErr("Must specify either --copy-in or --copy-out%s", "");
            do_die = 1;
        }
    }

    // unlink and close prior to exit - unlink is by qname, close by queue descriptor
    if (ul_flag) {
        if (mq_unlink(qname)) {
            SysErr("Unlink %s failed", "qname");
            do_die = 1;
        }
    }
    if (mq_close(mqd)) {
        SysErr("Close %d failed", (int)mqd);
        do_die = 1;
    }
    free(qname);

    /* Fail if something has failed previously */
    if (do_die) 
        exit(EXIT_FAILURE);
    else
        exit(0);
}

int copy_out(mqd_t mqd, int mlen, int tblk, int wtime) {
    int msglen = mlen, do_die = 0, nret = 0;
    char *mbuf;
    struct timespec abst;

    mbuf = (char*)malloc(msglen + 2);
    if (mbuf == NULL) {
        SysErr("Malloc failure%s", "");
        return 1;
    }
    while (1) {
        if (tblk) {             // time limited blocking
            if (ts2abs(&abst, wtime)) {
                SysErr("Setting wake time failed%s", "");
                free(mbuf);
                return 1;       // set do_die in caller
            } else {
                nret = (int)mq_timedreceive(mqd, mbuf, (size_t)msglen, NULL, &abst);
            }
        } else {                // just block forever
            nret = (int)mq_receive(mqd, mbuf, (size_t)msglen, NULL);
        }

        if (nret != -1) {
            mbuf[nret] = '\0';      // counted string to z-string
            fprintf(stdout, "%s", mbuf);
        } else {
            nret = errno;
            switch (nret) {
                case EAGAIN:
                        free(mbuf);
                        return 0;   // blocked maximum time on empty queue
                    break;
                case EBADF:
                        SysErr("Invalid queue descriptor %d", mqd);
                        free(mbuf);
                        return 1;
                    break;
                case EINTR:         // received signal
                        free(mbuf);
                        return 0;
                    break;
                case EINVAL:
                        SysErr("Invalid blocking time%s", "");
                        free(mbuf);
                        return 1;
                    break;
                case EMSGSIZE:      // message length too short
                        SysErr("Bad message length received%s", "");
                        free(mbuf);
                        return 1;
                    break;
                case ETIMEDOUT:     // call timed out during message transfer
                        free(mbuf);
                        return 0;   // consider that normal for a blocking queue
                    break;
                default:
                    SysErr("Unexpected error code%s", "");
                    free(mbuf);
                    return 1;
            }
        }
    }
}

int copy_in(mqd_t mqd, int mlen, int pri, int tblk, int wtime) {
    int msglen = mlen, nret, slen;
    char *mbuf;
    struct timespec abst;

    mbuf = (char*)malloc(msglen + 2);
    if (mbuf == NULL) {
        SysErr("Malloc failure%s", "");
        return 1;
    }

    while (!feof(stdin)) {
        if (fgets(mbuf, msglen, stdin) != NULL) {   /* at most, 1 byte less than msglen + '\0' */
            slen = (int)strlen(mbuf);
            if (tblk) {     /* Blocking time limited */
                if (ts2abs(&abst, wtime)){
                    SysErr("Setting wake time failed%s", "");
                    free(mbuf);
                    return 1;
                }
                nret = mq_timedsend(mqd, mbuf, slen, pri, &abst);
            } else {
                nret = mq_send(mqd, mbuf, slen, pri);
            }
            if (nret) {
                nret = errno;
                switch (nret) {
                    case EAGAIN:
                        free(mbuf);
                        return 0;   // blocked maximum time on full queue
                        break;
                    case EBADF:
                        SysErr("Invalid queue descriptor %d", mqd);
                        free(mbuf);
                        return 1;
                        break;
                    case EINTR:     // received signal
                        free(mbuf);
                        return 0;
                        break;
                    case EINVAL:
                        SysErr("Invalid blocking time%s", "");
                        free(mbuf);
                        return 1;
                        break;
                    case EMSGSIZE:  // message length too long
                        SysErr("Bad message length%s", "");
                        free(mbuf);
                        return 1;
                        break;
                    case ETIMEDOUT: // call timed out during message transfer
                        free(mbuf);
                        return 0;   // consider that normal for a blocking queue
                        break;
                    default:
                        SysErr("Unexpected error code%s", "");
                        free(mbuf);
                        return 1;
                }
            }
         } else {   /* NULL returned */
             if (errno != 0  && errno != EOF) {
                 SysErr("Get stream error%s", "");
                 free(mbuf);
                 return 1;
             } else {   /* EoF without characters being read */
                 free(mbuf);
                 return 0;
             }
         }
    }
    free(mbuf);
    return 0;
}

/* Update timespec structure by tenths of a second 
 * return non-zero on error
 * else ts strucure updated to the end of the delay time
 */
int ts2abs(struct timespec *ts, int tsec) {
    int ret;
    long wsec, wths, wtime = (long)tsec;

    ret = clock_gettime(CLOCK_REALTIME, ts);
    if (ret) return ret;

    wsec = wtime / 10;                          /* seconds */
    wths = wtime % 10;                          /* tenths */
    wths *= 100000000;                          /* as nano-seconds */
    ts->tv_nsec += wths;
    ts->tv_sec  += ts->tv_nsec / 1000000000;    /* carry any seconds */
    ts->tv_nsec %= 1000000000;                  /* just the nano-seconds */
    ts->tv_sec  += wsec;
    return 0;
}
