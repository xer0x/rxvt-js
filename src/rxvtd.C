/*----------------------------------------------------------------------*
 * File:	rxvtd.C
 *----------------------------------------------------------------------*
 *
 * All portions of code are copyright by their respective author/s.
 * Copyright (c) 2003-2007 Marc Lehmann <pcg@goof.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *----------------------------------------------------------------------*/

#include "../config.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>

#include "rxvt.h"
#include "rxvtdaemon.h"
#include "libptytty.h"

struct server : rxvt_connection {
  log_callback log_cb;
  getfd_callback getfd_cb;

  void read_cb (ev::io &w, int revents); ev::io read_ev;
  void log_msg (const char *msg);
  int getfd (int remote_fd);

  server (int fd)
  {
    read_ev.set <server, &server::read_cb> (this);
    log_cb.set  <server, &server::log_msg> (this);
    getfd_cb.set<server, &server::getfd>   (this);

    this->fd = fd;
    fcntl (fd, F_SETFD, FD_CLOEXEC);
    fcntl (fd, F_SETFL, 0);
    read_ev.start (fd, ev::READ);
  }

  void err (const char *format = 0, ...);
};

struct unix_listener {
  int fd;

  void accept_cb (ev::io &w, int revents); ev::io accept_ev;

  unix_listener (const char *sockname);
};

unix_listener::unix_listener (const char *sockname)
{
  accept_ev.set<unix_listener, &unix_listener::accept_cb> (this);

  sockaddr_un sa;

  if (strlen (sockname) >= sizeof(sa.sun_path))
    {
      fputs ("socket name too long, aborting.\n", stderr);
      exit (EXIT_FAILURE);
    }

  if ((fd = socket (AF_UNIX, SOCK_STREAM, 0)) < 0)
    {
      perror ("unable to create listening socket");
      exit (EXIT_FAILURE);
    }

  fcntl (fd, F_SETFD, FD_CLOEXEC);
  fcntl (fd, F_SETFL, O_NONBLOCK);

  sa.sun_family = AF_UNIX;
  strcpy (sa.sun_path, sockname);

  unlink (rxvt_connection::unix_sockname ());

  mode_t omask = umask (0077);

  if (bind (fd, (sockaddr *)&sa, sizeof (sa)))
    {
      perror ("unable to bind listening socket");
      exit (EXIT_FAILURE);
    }

  umask (omask);

  if (listen (fd, 5))
    {
      perror ("unable to listen on socket");
      exit (EXIT_FAILURE);
    }

  accept_ev.start (fd, ev::READ);
}

void unix_listener::accept_cb (ev::io &w, int revents)
{
  int fd2 = accept (fd, 0, 0);

  if (fd2 >= 0)
    new server (fd2);
}

int server::getfd (int remote_fd)
{
  send ("GETFD");
  send (remote_fd);
  return ptytty::recv_fd (fd);
}

void server::log_msg (const char *msg)
{
  send ("MSG"), send (msg);
}

void server::err (const char *format, ...)
{
  if (format)
    {
      char err[1024];

      va_list ap;
      va_start (ap, format);
      vsnprintf (err, 1024, format, ap);
      va_end (ap);

      log_msg (err);
    }

  send ("END"), send (0);
  close (fd);
  delete this;
}

void server::read_cb (ev::io &w, int revents)
{
  auto_str tok;

  if (recv (tok))
    {
      if (!strcmp (tok, "NEW"))
        {
          stringvec *argv = new stringvec;
          stringvec *envv = new stringvec;

          for (;;)
            {
              if (!recv (tok))
                return err ();

              if (!strcmp (tok, "END"))
                break;
              else if (!strcmp (tok, "ENV") && recv (tok))
                envv->push_back (strdup (tok));
              else if (!strcmp (tok, "ARG") && recv (tok))
                argv->push_back (strdup (tok));
              else
                return err ("protocol error: unexpected NEW token.\n");
            }

          envv->push_back (0);

          {
            rxvt_term *term = new rxvt_term;

            term->log_hook = &log_cb;
            term->getfd_hook = &getfd_cb;

            bool success = true;

            try
              {
                term->init (argv, envv);
              }
            catch (const class rxvt_failure_exception &e)
              {
                success = false;
              }

            term->log_hook = 0;

            chdir ("/"); // init might change to different working directory

            if (!success)
              term->destroy ();

            send ("END"); send (success ? 1 : 0);
          }
        }
      else
        return err ("protocol error: request '%s' unsupported.\n", (char *)tok);
    }
  else
    return err ();
}

int opt_fork, opt_opendisplay, opt_quiet;

int
main (int argc, const char *const *argv)
{
  rxvt_init ();

  for (int i = 1; i < argc; i++)
    {
      if (!strcmp (argv [i], "-f") || !strcmp (argv [i], "--fork"))
        opt_fork = 1;
      else if (!strcmp (argv [i], "-o") || !strcmp (argv [i], "--opendisplay"))
        opt_opendisplay = 1;
      else if (!strcmp (argv [i], "-q") || !strcmp (argv [i], "--quiet"))
        opt_quiet = 1;
      else
        {
          rxvt_log ("%s: unknown option '%s', aborting.\n", argv [0], argv [i]);
          return EXIT_FAILURE;
        }
    }

  // optionally open display and never release it.
  if (opt_opendisplay)
    if (const char *dpy = getenv ("DISPLAY"))
      displays.get (dpy ? dpy : ":0"); // move string logic into rxvt_display maybe?

  char *sockname = rxvt_connection::unix_sockname ();
  unix_listener l (sockname);

  chdir ("/");

  if (!opt_quiet)
    {
      printf ("rxvt-unicode daemon listening on %s.\n", sockname);
      fflush (stdout);
    }

  free (sockname);

  if (opt_fork)
    {
      pid_t pid = fork ();

      if (pid < 0)
        {
          rxvt_log ("unable to fork daemon, aborting.\n");
          return EXIT_FAILURE;
        }
      else if (pid > 0)
        _exit (EXIT_SUCCESS);

      ev_default_fork ();
    }

  ev_loop (0);

  return EXIT_SUCCESS;
}

