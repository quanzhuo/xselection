#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xmu/Atoms.h>

#include "xselection.h"

/* command line option table for XrmParseCommand() */
XrmOptionDescRec opt_tab[14];

/* Options that get set on the command line */
int sloop = 0;			 /* number of loops */
char *sdisp = NULL;		 /* X display to connect to */
Atom sseln = XA_PRIMARY; /* X selection to work with */
Atom target = XA_STRING;

/* Flags for command line options */
static int frmnl = 1; /* remove (single) newline character at the very end if present */

Display *dpy;			   /* connection to X11 display */
XrmDatabase opt_db = NULL; /* database for options */

char **fil_names;   /* names of files to read */
int fil_number = 0; /* number of files to read */
int fil_current = 0;
FILE *fil_handle = NULL;

/* variables to hold Xrm database record and type */
XrmValue rec_val;
char *rec_typ;

int tempi = 0;

/* Returns the machine-specific number of bytes per data element
 * returned by XGetWindowProperty */
static size_t
mach_itemsize(int format)
{
    if (format == 8)
	return sizeof(char);
    if (format == 16)
	return sizeof(short);
    if (format == 32)
	return sizeof(long);
    return 0;
}

/* failure message for malloc() problems */
void
errmalloc(void)
{
    fprintf(stderr, "Error: Could not allocate memory.\n");
    exit(EXIT_FAILURE);
}

/* check a pointer to allocater memory, print an error if it's null */
void
xcmemcheck(void *ptr)
{
    if (ptr == NULL)
	errmalloc();
}

/* wrapper for malloc that checks for errors */
void *
xcmalloc(size_t size)
{
    void *mem;

    mem = malloc(size);
    xcmemcheck(mem);

    return (mem);
}

/* wrapper for realloc that checks for errors */
void *
xcrealloc(void *ptr, size_t size)
{
    void *mem;

    mem = realloc(ptr, size);
    xcmemcheck(mem);

    return (mem);
}

static void
printSelBuf(FILE *fout, Atom sel_type, unsigned char *sel_buf, size_t sel_len)
{
#ifdef HAVE_ICONV
	Atom html = XInternAtom(dpy, "text/html", True);
#endif

	if (sel_type == XA_INTEGER)
	{
		/* if the buffer contains integers, print them */
		long *long_buf = (long *)sel_buf;
		size_t long_len = sel_len / sizeof(long);
		while (long_len--)
			fprintf(fout, "%ld\n", *long_buf++);
		return;
	}

	if (sel_type == XA_ATOM)
	{
		/* if the buffer contains atoms, print their names */
		Atom *atom_buf = (Atom *)sel_buf;
		size_t atom_len = sel_len / sizeof(Atom);
		while (atom_len--)
		{
			char *atom_name = XGetAtomName(dpy, *atom_buf++);
			fprintf(fout, "%s\n", atom_name);
			XFree(atom_name);
		}
		return;
	}

#ifdef HAVE_ICONV
	if (html != None && sel_type == html)
	{
		/* if the buffer contains UCS-2 (UTF-16), convert to
	 * UTF-8.  Mozilla-based browsers do this for the
	 * text/html target.
	 */
		iconv_t cd;
		char *sel_charset = NULL;
		if (sel_buf[0] == 0xFF && sel_buf[1] == 0xFE)
			sel_charset = "UTF-16LE";
		else if (sel_buf[0] == 0xFE && sel_buf[1] == 0xFF)
			sel_charset = "UTF-16BE";

		if (sel_charset != NULL && (cd = iconv_open("UTF-8", sel_charset)) != (iconv_t)-1)
		{
			char *out_buf_start = malloc(sel_len), *in_buf = (char *)sel_buf + 2,
				 *out_buf = out_buf_start;
			size_t in_bytesleft = sel_len - 2, out_bytesleft = sel_len;

			while (iconv(cd, &in_buf, &in_bytesleft, &out_buf, &out_bytesleft) == -1 && errno == E2BIG)
			{
				fwrite(out_buf_start, sizeof(char), sel_len - out_bytesleft, fout);
				out_buf = out_buf_start;
				out_bytesleft = sel_len;
			}
			if (out_buf != out_buf_start)
				fwrite(out_buf_start, sizeof(char), sel_len - out_bytesleft, fout);

			free(out_buf_start);
			iconv_close(cd);
			return;
		}
	}
#endif

	/* otherwise, print the raw buffer out */
	fwrite(sel_buf, sizeof(char), sel_len, fout);
}

int
xcout(Display * dpy,
      Window win,
      XEvent evt, Atom sel, Atom target, Atom * type, unsigned char **txt, unsigned long *len,
      unsigned int *context)
{
    /* a property for other windows to put their selection into */
    static Atom pty;
    static Atom inc;
    int pty_format;

    /* buffer for XGetWindowProperty to dump data into */
    unsigned char *buffer;
    unsigned long pty_size, pty_items, pty_machsize;

    /* local buffer of text to return */
    unsigned char *ltxt = *txt;

    if (!pty) {
	pty = XInternAtom(dpy, "XCLIP_OUT", False);
    }

    if (!inc) {
	inc = XInternAtom(dpy, "INCR", False);
    }

    switch (*context) {
	/* there is no context, do an XConvertSelection() */
    case XCLIB_XCOUT_NONE:
	/* initialise return length to 0 */
	if (*len > 0) {
	    free(*txt);
	    *len = 0;
	}

	/* send a selection request */
	XConvertSelection(dpy, sel, target, pty, win, CurrentTime);
	*context = XCLIB_XCOUT_SENTCONVSEL;
	return (0);

    case XCLIB_XCOUT_SENTCONVSEL:
	if (evt.type != SelectionNotify)
	    return (0);

	/* return failure when the current target failed */
	if (evt.xselection.property == None) {
	    *context = XCLIB_XCOUT_BAD_TARGET;
	    return (0);
	}

	/* find the size and format of the data in property */
	XGetWindowProperty(dpy,
			   win,
			   pty,
			   0,
			   0,
			   False,
			   AnyPropertyType, type, &pty_format, &pty_items, &pty_size, &buffer);
	XFree(buffer);

	if (*type == inc) {
	    /* start INCR mechanism by deleting property */
	    XDeleteProperty(dpy, win, pty);
	    XFlush(dpy);
	    *context = XCLIB_XCOUT_INCR;
	    return (0);
	}

	/* not using INCR mechanism, just read the property */
	XGetWindowProperty(dpy,
			   win,
			   pty,
			   0,
			   (long) pty_size,
			   False,
			   AnyPropertyType, type, &pty_format, &pty_items, &pty_size, &buffer);

	/* finished with property, delete it */
	XDeleteProperty(dpy, win, pty);

	/* compute the size of the data buffer we received */
	pty_machsize = pty_items * mach_itemsize(pty_format);

	/* copy the buffer to the pointer for returned data */
	ltxt = (unsigned char *) xcmalloc(pty_machsize);
	memcpy(ltxt, buffer, pty_machsize);

	/* set the length of the returned data */
	*len = pty_machsize;
	*txt = ltxt;

	/* free the buffer */
	XFree(buffer);

	*context = XCLIB_XCOUT_NONE;

	/* complete contents of selection fetched, return 1 */
	return (1);

    case XCLIB_XCOUT_INCR:
	/* To use the INCR method, we basically delete the
	 * property with the selection in it, wait for an
	 * event indicating that the property has been created,
	 * then read it, delete it, etc.
	 */

	/* make sure that the event is relevant */
	if (evt.type != PropertyNotify)
	    return (0);

	/* skip unless the property has a new value */
	if (evt.xproperty.state != PropertyNewValue)
	    return (0);

	/* check size and format of the property */
	XGetWindowProperty(dpy,
			   win,
			   pty,
			   0,
			   0,
			   False,
			   AnyPropertyType,
			   type, &pty_format, &pty_items, &pty_size, (unsigned char **) &buffer);

	if (pty_size == 0) {
	    /* no more data, exit from loop */
	    XFree(buffer);
	    XDeleteProperty(dpy, win, pty);
	    *context = XCLIB_XCOUT_NONE;

	    /* this means that an INCR transfer is now
	     * complete, return 1
	     */
	    return (1);
	}

	XFree(buffer);

	/* if we have come this far, the propery contains
	 * text, we know the size.
	 */
	XGetWindowProperty(dpy,
			   win,
			   pty,
			   0,
			   (long) pty_size,
			   False,
			   AnyPropertyType,
			   type, &pty_format, &pty_items, &pty_size, (unsigned char **) &buffer);

	/* compute the size of the data buffer we received */
	pty_machsize = pty_items * mach_itemsize(pty_format);

	/* allocate memory to accommodate data in *txt */
	if (*len == 0) {
	    *len = pty_machsize;
	    ltxt = (unsigned char *) xcmalloc(*len);
	}
	else {
	    *len += pty_machsize;
	    ltxt = (unsigned char *) xcrealloc(ltxt, *len);
	}

	/* add data to ltxt */
	memcpy(&ltxt[*len - pty_machsize], buffer, pty_machsize);

	*txt = ltxt;
	XFree(buffer);

	/* delete property to get the next item */
	XDeleteProperty(dpy, win, pty);
	XFlush(dpy);
	return (0);
    }

    return (0);
}

static int
doOut(Window win)
{
	Atom sel_type = None;
	unsigned char *sel_buf;	/* buffer for selection data */
	unsigned long sel_len = 0; /* length of sel_buf */
	XEvent evt;				   /* X Event Structures */
	unsigned int context = XCLIB_XCOUT_NONE;

	if (sseln == XA_STRING)
		sel_buf = (unsigned char *)XFetchBuffer(dpy, (int *)&sel_len, 0);
	else
	{
		while (1)
		{
			/* only get an event if xcout() is doing something */
			if (context != XCLIB_XCOUT_NONE)
				XNextEvent(dpy, &evt);

			/* fetch the selection, or part of it */
			xcout(dpy, win, evt, sseln, target, &sel_type, &sel_buf, &sel_len, &context);

			if (context == XCLIB_XCOUT_BAD_TARGET)
			{
				if (target == XA_UTF8_STRING(dpy))
				{
					/* fallback is needed. set XA_STRING to target and restart the loop. */
					context = XCLIB_XCOUT_NONE;
					target = XA_STRING;
					continue;
				}
				else
				{
					/* no fallback available, exit with failure */
					char *atom_name = XGetAtomName(dpy, target);
					fprintf(stderr, "Error: target %s not available\n", atom_name);
					XFree(atom_name);
					return EXIT_FAILURE;
				}
			}

			/* only continue if xcout() is doing something */
			if (context == XCLIB_XCOUT_NONE)
				break;
		}
	}

	/* remove the last newline character if necessary */
	if (frmnl && sel_len && sel_buf[sel_len - 1] == '\n')
	{
		sel_len--;
	}

	if (sel_len)
	{
		/* only print the buffer out, and free it, if it's not
	 * empty
	 */
		printSelBuf(stdout, sel_type, sel_buf, sel_len);
		printf("\n");
		if (sseln == XA_STRING)
			XFree(sel_buf);
		else
			free(sel_buf);
	}

	return EXIT_SUCCESS;
}

/* failure to connect to X11 display */
void errxdisplay(char *display)
{
	/* if the display wasn't specified, read it from the environment
     * just like XOpenDisplay would
     */
	if (display == NULL)
		display = getenv("DISPLAY");

	fprintf(stderr, "Error: Can't open display: %s\n", display ? display : "(null)");
	exit(EXIT_FAILURE);
}

/* process noutf8 and target command line options */
static void
doOptTarget(void)
{
	/* check for -noutf8 */
	if (XrmGetResource(opt_db, "xclip.noutf8", "Xclip.noutf8", &rec_typ, &rec_val))
	{
		//if (fverb == OVERBOSE) /* print in verbose mode only */
		//	fprintf(stderr, "Using old UNICODE instead of UTF8.\n");
	}
	else if (XrmGetResource(opt_db, "xclip.target", "Xclip.Target", &rec_typ, &rec_val))
	{
		target = XInternAtom(dpy, rec_val.addr, False);
		//if (fverb == OVERBOSE) /* print in verbose mode only */
		//	fprintf(stderr, "Using %s.\n", rec_val.addr);
	}
	else
	{
		target = XA_UTF8_STRING(dpy);
		//if (fverb == OVERBOSE) /* print in verbose mode only */
		//	fprintf(stderr, "Using UTF8_STRING.\n");
	}
}

int main(int argc, char *argv[])
{
	/* Declare variables */
	Window win; /* Window */
	int exit_code;

	/* Connect to the X server. */
	if ((dpy = XOpenDisplay(sdisp)))
	{
		/* successful */
	}
	else
	{
		/* couldn't connect to X server. Print error and exit */
		errxdisplay(sdisp);
	}

	/* parse selection command line option */
	// doOptSel();

	/* parse noutf8 and target command line options */
	doOptTarget();

	/* Create a window to trap events */
	win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);

	/* get events about property changes */
	XSelectInput(dpy, win, PropertyChangeMask);

	exit_code = doOut(win);

	/* Disconnect from the X server */
	XCloseDisplay(dpy);

	/* exit */
	return exit_code;
}
