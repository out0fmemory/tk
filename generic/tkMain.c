/*
 * tkMain.c --
 *
 *	This file contains a generic main program for Tk-based applications.
 *	It can be used as-is for many applications, just by supplying a
 *	different appInitProc function for each specific application. Or, it
 *	can be used as a template for creating new main programs for Tk
 *	applications.
 *
 * Copyright (c) 1990-1994 The Regents of the University of California.
 * Copyright (c) 1994-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

/**
 * On Windows, this file needs to be compiled twice, once with
 * TK_ASCII_MAIN defined. This way both Tk_MainEx and Tk_MainExW
 * can be implemented, sharing the same source code.
 */
#if defined(TK_ASCII_MAIN)
#   ifdef UNICODE
#	undef UNICODE
#	undef _UNICODE
#   else
#	define UNICODE
#	define _UNICODE
#   endif
#endif

#include "tkInt.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#ifdef NO_STDLIB_H
#   include "../compat/stdlib.h"
#else
#   include <stdlib.h>
#endif

extern int TkCygwinMainEx(int, char **, Tcl_AppInitProc *, Tcl_Interp *);

/*
 * The default prompt used when the user has not overridden it.
 */

#define DEFAULT_PRIMARY_PROMPT	"% "

/*
 * This file can be compiled on Windows in UNICODE mode, as well as
 * on all other platforms using the native encoding. This is done
 * by using the normal Windows functions like _tcscmp, but on
 * platforms which don't have <tchar.h> we have to translate that
 * to strcmp here.
 */
#ifdef _WIN32
/*  Little hack to eliminate the need for "tclInt.h" here:
    Just copy a small portion of TclIntPlatStubs, just
    enough to make it work. See [600b72bfbc] */
typedef struct {
    int magic;
    void *hooks;
    void (*dummy[16]) (void); /* dummy entries 0-15, not used */
    int (*tclpIsAtty) (int fd); /* 16 */
} TclIntPlatStubs;
extern const TclIntPlatStubs *tclIntPlatStubsPtr;
#   include "tkWinInt.h"
#else
#   define TCHAR char
#   define TEXT(arg) arg
#   define _tcscmp strcmp
#   define _tcslen strlen
#   define _tcsncmp strncmp
#endif
#ifdef ZIPFS_IN_TCL
#   include "zipfs.h"
#endif

#ifdef MAC_OSX_TK
#include "tkMacOSXInt.h"
#endif

#ifdef PLATFORM_SDL
#include <SDL2/SDL.h>
#endif

#ifdef ANDROID
#undef  ZIPFS_BOOTDIR
#define ZIPFS_BOOTDIR "/assets"
#endif

/*
 * Further on, in UNICODE mode, we need to use Tcl_NewUnicodeObj,
 * while otherwise NewNativeObj is needed (which provides proper
 * conversion from native encoding to UTF-8).
 */
#ifdef UNICODE
#   define NewNativeObj Tcl_NewUnicodeObj
#else /* !UNICODE */
    static Tcl_Obj *NewNativeObj(char *string, int length) {
	Tcl_Obj *obj;
	Tcl_DString ds;
	Tcl_ExternalToUtfDString(NULL, string, length, &ds);
	obj = Tcl_NewStringObj(Tcl_DStringValue(&ds), Tcl_DStringLength(&ds));
	Tcl_DStringFree(&ds);
	return obj;
}
#endif /* !UNICODE */

/*
 * Declarations for various library functions and variables (don't want to
 * include tkInt.h or tkPort.h here, because people might copy this file out
 * of the Tk source directory to make their own modified versions). Note: do
 * not declare "exit" here even though a declaration is really needed, because
 * it will conflict with a declaration elsewhere on some systems.
 */

#if defined(_WIN32)
#define isatty WinIsTty
static int WinIsTty(int fd) {
    HANDLE handle;

    /*
     * For now, under Windows, we assume we are not running as a console mode
     * app, so we need to use the GUI console. In order to enable this, we
     * always claim to be running on a tty. This probably isn't the right way
     * to do it.
     */

#if !defined(STATIC_BUILD)
	if (tclStubsPtr->reserved9 && tclIntPlatStubsPtr->tclpIsAtty) {
	    /* We are running on Cygwin */
	    return tclIntPlatStubsPtr->tclpIsAtty(fd);
	}
#endif
    handle = GetStdHandle(STD_INPUT_HANDLE + fd);
	/*
	 * If it's a bad or closed handle, then it's been connected to a wish
	 * console window. A character file handle is a tty by definition.
	 */
    return (handle == INVALID_HANDLE_VALUE) || (handle == 0)
	     || (GetFileType(handle) == FILE_TYPE_UNKNOWN)
	     || (GetFileType(handle) == FILE_TYPE_CHAR);
}
#else
extern int		isatty(int fd);
#endif

typedef struct InteractiveState {
    Tcl_Channel input;		/* The standard input channel from which lines
				 * are read. */
    int tty;			/* Non-zero means standard input is a
				 * terminal-like device. Zero means it's a
				 * file. */
    Tcl_DString command;	/* Used to assemble lines of terminal input
				 * into Tcl commands. */
    Tcl_DString line;		/* Used to read the next line from the
				 * terminal input. */
    int gotPartial;
    Tcl_Interp *interp;		/* Interpreter that evaluates interactive
				 * commands. */
} InteractiveState;

/*
 * Forward declarations for functions defined later in this file.
 */

static void		Prompt(Tcl_Interp *interp, InteractiveState *isPtr);
static void		StdinProc(ClientData clientData, int mask);

/*
 *----------------------------------------------------------------------
 *
 * Tk_MainEx --
 *
 *	Main program for Wish and most other Tk-based applications.
 *
 * Results:
 *	None. This function never returns (it exits the process when it's
 *	done).
 *
 * Side effects:
 *	This function initializes the Tk world and then starts interpreting
 *	commands; almost anything could happen, depending on the script being
 *	interpreted.
 *
 *----------------------------------------------------------------------
 */

void
Tk_MainEx(
    int argc,			/* Number of arguments. */
    TCHAR **argv,		/* Array of argument strings. */
    Tcl_AppInitProc *appInitProc,
				/* Application-specific initialization
				 * function to call after most initialization
				 * but before starting to execute commands. */
    Tcl_Interp *interp)
{
    Tcl_Obj *path, *argvPtr, *appName;
    const char *encodingName;
    int code, nullStdin = 0;
    Tcl_Channel chan;
    InteractiveState is;
#ifdef ZIPFS_IN_TCL
    const char *zipFile = NULL;
    Tcl_Obj *zipval = NULL;
    int autoRun = 1;
    int zipOk = TCL_ERROR;
#ifdef ANDROID
    const char *zipFile2 = NULL;
#else
    const char *exeName;
    Tcl_DString systemEncodingName;
#endif
#endif

    /*
     * Ensure that we are getting a compatible version of Tcl.
     */

    if (Tcl_InitStubs(interp, "8.6", 0) == NULL) {
	if (Tcl_InitStubs(interp, "8.1", 0) == NULL) {
	    abort();
	} else {
	    Tcl_Panic("%s", Tcl_GetString(Tcl_GetObjResult(interp)));
	}
    }

#if defined(_WIN32) && !defined(UNICODE) && !defined(STATIC_BUILD)

    if (tclStubsPtr->reserved9) {
	/* We are running win32 Tk under Cygwin, so let's check
	 * whether the env("DISPLAY") variable or the -display
	 * argument is set. If so, we really want to run the
	 * Tk_MainEx function of libtk8.?.dll, not this one. */
	if (Tcl_GetVar2(interp, "env", "DISPLAY", TCL_GLOBAL_ONLY)) {
	loadCygwinTk:
	    if (TkCygwinMainEx(argc, argv, appInitProc, interp)) {
		/* Should never reach here. */
		return;
	    }
	} else {
	    int i;

	    for (i = 1; i < argc; ++i) {
		if (!_tcscmp(argv[i], TEXT("-display"))) {
		    goto loadCygwinTk;
		}
	    }
	}
    }
#endif

#if defined(ZIPFS_IN_TCL) && !defined(ANDROID)
    exeName = Tcl_GetNameOfExecutable();
#endif

    Tcl_InitMemory(interp);

    is.interp = interp;
    is.gotPartial = 0;
    Tcl_Preserve(interp);

#ifdef PLATFORM_SDL
    Tk_InitConsoleChannels(interp);
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
    Tk_InitConsoleChannels(interp);
#endif

#ifdef MAC_OSX_TK
    if (Tcl_GetStartupScript(NULL) == NULL) {
	TkMacOSXDefaultStartupScript();
    }
#endif

    /*
     * If the application has not already set a startup script, parse the
     * first few command line arguments to determine the script path and
     * encoding.
     */

    if (NULL == Tcl_GetStartupScript(NULL)) {
	size_t length;

	/*
	 * Check whether first 3 args (argv[1] - argv[3]) look like
	 *  -encoding ENCODING FILENAME
	 * or like
	 *  FILENAME
	 * or like
	 *  -file FILENAME		(ancient history support only)
	 */

	if ((argc > 3) && (0 == _tcscmp(TEXT("-encoding"), argv[1]))
		&& (TEXT('-') != argv[3][0])) {
		Tcl_Obj *value = NewNativeObj(argv[2], -1);
	    Tcl_SetStartupScript(NewNativeObj(argv[3], -1), Tcl_GetString(value));
	    Tcl_DecrRefCount(value);
	    argc -= 3;
	    argv += 3;
        } else if (argc > 1) {
	    length = strlen((char *) argv[1]);
#ifdef ZIPFS_IN_TCL
	    if ((length >= 2) &&
		(0 == _tcsncmp(TEXT("-zip"), argv[1], length))) {
		argc--;
		argv++;
		if ((argc > 1) && (argv[1][0] != (TCHAR) '-')) {
		    zipval = NewNativeObj(argv[1], -1);
		    zipFile = Tcl_GetString(zipval);
		    autoRun = 0;
		    argc--;
		    argv++;
		}
	    } else
#endif
	    if (TEXT('-') != argv[1][0]) {
		Tcl_SetStartupScript(NewNativeObj(argv[1], -1), NULL);
		argc--;
		argv++;
	    }
	} else if ((argc > 2) && (length = _tcslen(argv[1]))
		&& (length > 1) && (0 == _tcsncmp(TEXT("-file"), argv[1], length))
		&& (TEXT('-') != argv[2][0])) {
	    Tcl_SetStartupScript(NewNativeObj(argv[2], -1), NULL);
	    argc -= 2;
	    argv += 2;
	}
    }

    path = Tcl_GetStartupScript(&encodingName);
    if (path == NULL) {
	appName = NewNativeObj(argv[0], -1);
    } else {
	appName = path;
    }
    Tcl_SetVar2Ex(interp, "argv0", NULL, appName, TCL_GLOBAL_ONLY);
    argc--;
    argv++;

    Tcl_SetVar2Ex(interp, "argc", NULL, Tcl_NewIntObj(argc), TCL_GLOBAL_ONLY);

    argvPtr = Tcl_NewListObj(0, NULL);
    while (argc--) {
	Tcl_ListObjAppendElement(NULL, argvPtr, NewNativeObj(*argv++, -1));
    }
    Tcl_SetVar2Ex(interp, "argv", NULL, argvPtr, TCL_GLOBAL_ONLY);

    /*
     * Set the "tcl_interactive" variable.
     */

#ifdef PLATFORM_SDL
    is.tty = 1;
#else
    is.tty = isatty(0);
#endif
#if defined(MAC_OSX_TK)
    /*
     * On TkAqua, if we don't have a TTY and stdin is a special character file
     * of length 0, (e.g. /dev/null, which is what Finder sets when double
     * clicking Wish) then use the GUI console.
     */

    if (!is.tty) {
	struct stat st;

	nullStdin = fstat(0, &st) || (S_ISCHR(st.st_mode) && !st.st_blocks);
    }
#endif
    Tcl_SetVar2Ex(interp, "tcl_interactive", NULL,
	    Tcl_NewIntObj(!path && (is.tty || nullStdin)), TCL_GLOBAL_ONLY);

#ifdef ZIPFS_IN_TCL
    zipOk = Tclzipfs_Init(interp);
    if (zipOk == TCL_OK) {
	int relax = 0;

	if (zipFile == NULL) {
	    relax = 1;
#ifdef ANDROID
	    zipFile = getenv("TK_TCL_WISH_PACKAGE_CODE_PATH");
	    zipFile2 = getenv("PACKAGE_CODE_PATH");
	    if (zipFile == NULL) {
		zipFile = zipFile2;
		zipFile2 = NULL;
	    }
#else
	    zipFile = exeName;
#endif
	}
	if (zipFile != NULL) {
#ifdef ANDROID
	    zipOk = Tclzipfs_Mount(interp, zipFile, "", NULL);
#else
	    zipOk = Tclzipfs_Mount(interp, zipFile, exeName, NULL);
#endif
	    if (!relax && (zipOk != TCL_OK)) {
		Tcl_Exit(1);
	    }
#ifdef ANDROID
	    if (zipFile2 != NULL) {
		zipOk = Tclzipfs_Mount(interp, zipFile2, "/assets", NULL);
		if (zipOk != TCL_OK) {
		    Tcl_Exit(1);
		}
	    }
#endif
	} else {
	    zipOk = TCL_ERROR;
	}
	Tcl_ResetResult(interp);
    }
    if (zipOk == TCL_OK) {
	char *tk_lib;
	Tcl_DString dsTk;
#ifdef ZIPFS_BOOTDIR
	char *tcl_lib = ZIPFS_BOOTDIR "/tcl" TCL_VERSION;
	char *tcl_pkg = ZIPFS_BOOTDIR;
#else
	char *tcl_lib;
	char *tcl_pkg = (char *) exeName;
	Tcl_DString dsTcl;

	Tcl_DStringInit(&dsTcl);
	Tcl_DStringAppend(&dsTcl, exeName, -1);
	Tcl_DStringAppend(&dsTcl, "/tcl" TCL_VERSION, -1);
	tcl_lib = Tcl_DStringValue(&dsTcl);
#endif
	Tcl_SetVar2(interp, "env", "TCL_LIBRARY", tcl_lib, TCL_GLOBAL_ONLY);
	Tcl_SetVar(interp, "tcl_libPath", tcl_lib, TCL_GLOBAL_ONLY);
	Tcl_SetVar(interp, "tcl_library", tcl_lib, TCL_GLOBAL_ONLY);
	Tcl_SetVar(interp, "tcl_pkgPath", tcl_pkg, TCL_GLOBAL_ONLY);
	Tcl_SetVar(interp, "auto_path", tcl_lib,
		   TCL_GLOBAL_ONLY | TCL_LIST_ELEMENT);

	Tcl_DStringInit(&dsTk);
#ifdef ZIPFS_BOOTDIR
	Tcl_DStringSetLength(&dsTk, strlen(ZIPFS_BOOTDIR) + 32);
	Tcl_DStringSetLength(&dsTk, 0);
	tk_lib = Tcl_DStringValue(&dsTk);
#ifdef PLATFORM_SDL
        if (SDL_MAJOR_VERSION > 1) {
	    sprintf(tk_lib, ZIPFS_BOOTDIR "/sdl%dtk" TK_VERSION,
		    SDL_MAJOR_VERSION);
        } else {
	    strcpy(tk_lib, ZIPFS_BOOTDIR "/sdltk" TK_VERSION);
        }
#else
	strcpy(tk_lib, ZIPFS_BOOTDIR "/tk" TK_VERSION);
#endif
#else
	Tcl_DStringSetLength(&dsTk, strlen(exeName) + 32);
	Tcl_DStringSetLength(&dsTk, 0);
	tk_lib = Tcl_DStringValue(&dsTk);
#ifdef PLATFORM_SDL
        if (SDL_MAJOR_VERSION > 1) {
	    sprintf(tk_lib, "%s/sdl%dtk" TK_VERSION,
		    exeName, SDL_MAJOR_VERSION);
        } else {
	    strcpy(tk_lib, "%s/sdltk" TK_VERSION, exeName);
        }
#else
	sprintf(tk_lib, "%s/tk" TK_VERSION, exeName);
#endif
#endif
	Tcl_DStringSetLength(&dsTk, strlen(tk_lib));
        Tcl_SetVar2(interp, "env", "TK_LIBRARY", tk_lib, TCL_GLOBAL_ONLY);
        Tcl_SetVar(interp, "tk_library", tk_lib, TCL_GLOBAL_ONLY);

	Tcl_DStringFree(&dsTk);
#ifndef ZIPFS_BOOTDIR
	Tcl_DStringFree(&dsTcl);
#endif

	if (autoRun) {
	    char *filename;
	    Tcl_Channel chan;
#ifndef ZIPFS_BOOTDIR
	    Tcl_DString dsFilename;

	    Tcl_DStringInit(&dsFilename);
#endif

	    /*
 	     * Reset tcl_interactive to false if we'll later
     	     * source a file from ZIP, otherwise the console
     	     * will be displayed.
     	     */
#ifdef ANDROID
	    if (zipFile2 != NULL) {
		filename = ZIPFS_BOOTDIR "/assets/app/main.tcl";
		chan = Tcl_OpenFileChannel(NULL, filename, "r", 0);
	    } else
#endif
	    {
#ifdef ZIPFS_BOOTDIR
		filename = ZIPFS_BOOTDIR "/app/main.tcl";
#else
		Tcl_DStringAppend(&dsFilename, exeName, -1);
		Tcl_DStringAppend(&dsFilename, "/app/main.tcl", -1);
		filename = Tcl_DStringValue(&dsFilename);
#endif
		chan = Tcl_OpenFileChannel(NULL, filename, "r", 0);
	    }
	    if (chan != (Tcl_Channel) NULL) {
		const char *arg = NULL;

		Tcl_Close(NULL, chan);

		/*
		 * Push back script file to argv, if any.
 		 */
		if (path != NULL) {
		    arg = Tcl_GetString(path);
		}
		if (arg != NULL) {
		    Tcl_Obj *v, *no;

		    no = Tcl_NewStringObj("argv", 4);
		    v = Tcl_ObjGetVar2(interp, no, NULL, TCL_GLOBAL_ONLY);
		    if (v != NULL) {
			Tcl_Obj **objv, *n, *nv;
			int objc, i;

			objc = 0;
			Tcl_ListObjGetElements(NULL, v, &objc, &objv);
			n = Tcl_NewStringObj(arg, -1);
			nv = Tcl_NewListObj(1, &n);
			for (i = 0; i < objc; i++) {
			    Tcl_ListObjAppendElement(NULL, nv, objv[i]);
			}
			Tcl_IncrRefCount(nv);
			if (Tcl_ObjSetVar2(interp, no, NULL, nv,
					   TCL_GLOBAL_ONLY) != NULL) {
			    Tcl_GlobalEval(interp, "incr argc");
			}
			Tcl_DecrRefCount(nv);
		    }
		    Tcl_DecrRefCount(no);
		}
		Tcl_SetStartupScript(Tcl_NewStringObj(filename, -1), NULL);
		Tcl_SetVar(interp, "argv0", filename, TCL_GLOBAL_ONLY);
		Tcl_SetVar(interp, "tcl_interactive", "0", TCL_GLOBAL_ONLY);
	    } else {
		autoRun = 0;
	    }
#ifndef ZIPFS_BOOTDIR
	    Tcl_DStringFree(&dsFilename);
#endif
	}
    }

    if (zipval != NULL) {
        Tcl_DecrRefCount(zipval);
        zipval = NULL;
    }
#endif

    /*
     * Invoke application-specific initialization.
     */

    if (appInitProc(interp) != TCL_OK) {
	TkpDisplayWarning(Tcl_GetString(Tcl_GetObjResult(interp)),
		"application-specific initialization failed");
    }

#ifdef ZIPFS_IN_TCL
    /*
     * Setup auto loading info to point to mounted ZIP file.
     */

    if (zipOk == TCL_OK) {
#ifdef ZIPFS_BOOTDIR
	const char *tcl_lib = ZIPFS_BOOTDIR "/tcl" TCL_VERSION;
	const char *tcl_pkg = ZIPFS_BOOTDIR;
#else
	char *tcl_lib;
	char *tcl_pkg = (char *) exeName;
	Tcl_DString dsLib;

	Tcl_DStringInit(&dsLib);
	Tcl_DStringAppend(&dsLib, exeName, -1);
	Tcl_DStringAppend(&dsLib, "/tcl" TCL_VERSION, -1);
	tcl_lib = Tcl_DStringValue(&dsLib);
#endif

	Tcl_SetVar(interp, "tcl_libPath", tcl_lib, TCL_GLOBAL_ONLY);
	Tcl_SetVar(interp, "tcl_library", tcl_lib, TCL_GLOBAL_ONLY);
        Tcl_SetVar(interp, "tcl_pkgPath", tcl_pkg, TCL_GLOBAL_ONLY);
#ifndef ZIPFS_BOOTDIR
	Tcl_DStringFree(&dsLib);
#endif

	/*
	 * We need to set the system encoding (after initializing Tcl),
 	 * otherwise "encoding system" will return "identity"
 	 */

#ifdef ANDROID
	Tcl_SetSystemEncoding(NULL, "utf-8");
#else
	Tcl_SetSystemEncoding(NULL,
		Tcl_GetEncodingNameFromEnvironment(&systemEncodingName));
	Tcl_DStringFree(&systemEncodingName);
#endif
    }
#endif

    /*
     * Invoke the script specified on the command line, if any. Must fetch it
     * again, as the appInitProc might have reset it.
     */

    path = Tcl_GetStartupScript(&encodingName);
    if (path != NULL) {
#ifdef ZIPFS_IN_TCL
	const char *filename = Tcl_GetString(path);
	int length = strlen(filename);

	if ((length > 6) && (strncmp(filename, "zipfs:", 6) == 0)) {
	    Tcl_Obj *newPath;

	    zipOk = Tclzipfs_Mount(interp, filename + 6, "/app", NULL);
	    if (zipOk == TCL_OK) {
		newPath = Tcl_NewStringObj("/app/main.tcl", -1);
		Tcl_IncrRefCount(newPath);
		if (Tcl_FSAccess(newPath, R_OK) == 0) {
		    Tcl_SetStartupScript(newPath, encodingName);
		    path = newPath;
		    goto doit;
		}
		Tcl_DecrRefCount(newPath);
		newPath = Tcl_NewStringObj("/app/app/main.tcl", -1);
		Tcl_IncrRefCount(newPath);
		if (Tcl_FSAccess(newPath, R_OK) == 0) {
		    Tcl_SetStartupScript(newPath, encodingName);
		    path = newPath;
		    goto doit;
		}
		Tcl_DecrRefCount(newPath);
		newPath = Tcl_NewStringObj("/app/assets/app/main.tcl", -1);
		Tcl_IncrRefCount(newPath);
		if (Tcl_FSAccess(newPath, R_OK) == 0) {
		    Tcl_SetStartupScript(newPath, encodingName);
		    path = newPath;
		    goto doit;
		}
		Tcl_DecrRefCount(newPath);
		Tclzipfs_Unmount(interp, filename + 6);
	    }
	}
doit:
#endif
	Tcl_ResetResult(interp);
	code = Tcl_FSEvalFileEx(interp, path, encodingName);
	if (code != TCL_OK) {
	    /*
	     * The following statement guarantees that the errorInfo variable
	     * is set properly.
	     */

	    Tcl_AddErrorInfo(interp, "");
	    TkpDisplayWarning(Tcl_GetVar2(interp, "errorInfo", NULL,
		    TCL_GLOBAL_ONLY), "Error in startup script");
	    Tcl_DeleteInterp(interp);
	    Tcl_Exit(1);
	}
	is.tty = 0;
    } else {

	/*
	 * Evaluate the .rc file, if one has been specified.
	 */

	Tcl_SourceRCFile(interp);

	/*
	 * Establish a channel handler for stdin.
	 */

	is.input = Tcl_GetStdChannel(TCL_STDIN);
	if (is.input) {
	    Tcl_CreateChannelHandler(is.input, TCL_READABLE, StdinProc, &is);
	}
	if (is.tty) {
	    Prompt(interp, &is);
	}
    }

    chan = Tcl_GetStdChannel(TCL_STDOUT);
    if (chan) {
	Tcl_Flush(chan);
    }
    Tcl_DStringInit(&is.command);
    Tcl_DStringInit(&is.line);
    Tcl_ResetResult(interp);

    /*
     * Loop infinitely, waiting for commands to execute. When there are no
     * windows left, Tk_MainLoop returns and we exit.
     */

    Tk_MainLoop();
    Tcl_DeleteInterp(interp);
    Tcl_Release(interp);
    Tcl_SetStartupScript(NULL, NULL);
    Tcl_Exit(0);
}

/*
 *----------------------------------------------------------------------
 *
 * StdinProc --
 *
 *	This function is invoked by the event dispatcher whenever standard
 *	input becomes readable. It grabs the next line of input characters,
 *	adds them to a command being assembled, and executes the command if
 *	it's complete.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Could be almost arbitrary, depending on the command that's typed.
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
static void
StdinProc(
    ClientData clientData,	/* The state of interactive cmd line */
    int mask)			/* Not used. */
{
    char *cmd;
    int code, count;
    InteractiveState *isPtr = clientData;
    Tcl_Channel chan = isPtr->input;
    Tcl_Interp *interp = isPtr->interp;

    count = Tcl_Gets(chan, &isPtr->line);

    if (count < 0 && !isPtr->gotPartial) {
	if (isPtr->tty) {
	    Tcl_Exit(0);
	} else {
	    Tcl_DeleteChannelHandler(chan, StdinProc, isPtr);
	}
	return;
    }

    Tcl_DStringAppend(&isPtr->command, Tcl_DStringValue(&isPtr->line), -1);
    cmd = Tcl_DStringAppend(&isPtr->command, "\n", -1);
    Tcl_DStringFree(&isPtr->line);
    if (!Tcl_CommandComplete(cmd)) {
	isPtr->gotPartial = 1;
	goto prompt;
    }
    isPtr->gotPartial = 0;

    /*
     * Disable the stdin channel handler while evaluating the command;
     * otherwise if the command re-enters the event loop we might process
     * commands from stdin before the current command is finished. Among other
     * things, this will trash the text of the command being evaluated.
     */

    Tcl_CreateChannelHandler(chan, 0, StdinProc, isPtr);
    code = Tcl_RecordAndEval(interp, cmd, TCL_EVAL_GLOBAL);

    isPtr->input = Tcl_GetStdChannel(TCL_STDIN);
    if (isPtr->input) {
	Tcl_CreateChannelHandler(isPtr->input, TCL_READABLE, StdinProc, isPtr);
    }
    Tcl_DStringFree(&isPtr->command);
    if (Tcl_GetString(Tcl_GetObjResult(interp))[0] != '\0') {
	if ((code != TCL_OK) || (isPtr->tty)) {
	    chan = Tcl_GetStdChannel((code != TCL_OK) ? TCL_STDERR : TCL_STDOUT);
	    if (chan) {
		Tcl_WriteObj(chan, Tcl_GetObjResult(interp));
		Tcl_WriteChars(chan, "\n", 1);
	    }
	}
    }

    /*
     * If a tty stdin is still around, output a prompt.
     */

  prompt:
    if (isPtr->tty && (isPtr->input != NULL)) {
	Prompt(interp, isPtr);
    }
    Tcl_ResetResult(interp);
}

/*
 *----------------------------------------------------------------------
 *
 * Prompt --
 *
 *	Issue a prompt on standard output, or invoke a script to issue the
 *	prompt.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A prompt gets output, and a Tcl script may be evaluated in interp.
 *
 *----------------------------------------------------------------------
 */

static void
Prompt(
    Tcl_Interp *interp,		/* Interpreter to use for prompting. */
    InteractiveState *isPtr) /* InteractiveState. */
{
    Tcl_Obj *promptCmdPtr;
    int code;
    Tcl_Channel chan;

    promptCmdPtr = Tcl_GetVar2Ex(interp,
	isPtr->gotPartial ? "tcl_prompt2" : "tcl_prompt1", NULL, TCL_GLOBAL_ONLY);
    if (promptCmdPtr == NULL) {
    defaultPrompt:
	if (!isPtr->gotPartial) {
	    chan = Tcl_GetStdChannel(TCL_STDOUT);
	    if (chan != NULL) {
		Tcl_WriteChars(chan, DEFAULT_PRIMARY_PROMPT,
			strlen(DEFAULT_PRIMARY_PROMPT));
	    }
	}
    } else {
	code = Tcl_EvalObjEx(interp, promptCmdPtr, TCL_EVAL_GLOBAL);
	if (code != TCL_OK) {
	    Tcl_AddErrorInfo(interp,
		    "\n    (script that generates prompt)");
	    if (Tcl_GetString(Tcl_GetObjResult(interp))[0] != '\0') {
		chan = Tcl_GetStdChannel(TCL_STDERR);
		if (chan != NULL) {
		    Tcl_WriteObj(chan, Tcl_GetObjResult(interp));
		    Tcl_WriteChars(chan, "\n", 1);
		}
	    }
	    goto defaultPrompt;
	}
    }

    chan = Tcl_GetStdChannel(TCL_STDOUT);
    if (chan != NULL) {
	Tcl_Flush(chan);
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
