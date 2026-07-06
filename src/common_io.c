/*
===========================================================================
    Copyright (C) 2010-2013  Ninja and TheKelm
    Copyright (C) 1999-2005 Id Software, Inc.

    This file is part of CoD4X18-Server source code.

    CoD4X18-Server source code is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    CoD4X18-Server source code is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>
===========================================================================
*/



#include "q_shared.h"
#include "sys_thread.h"
#include "qcommon_io.h"
#include "qcommon_logprint.h"
#include "qcommon.h"
#include "sys_main.h"
#include <string.h>
#include <time.h>
#include <stdarg.h>


#ifndef __SYS_THREAD_H__
void Sys_EnterCriticalSection(int section){}
void Sys_LeaveCriticalSection(int section){}
#endif

#ifndef __SYS_MAIN_H__
#define Sys_Print(x) fputs(x, stdout);
#pragma message "Undefined function: Sys_Print"
#endif

#ifndef __QCOMMON_LOGPRINT_H__
void Com_PrintLogfile( const char* msg ){}
#pragma message "Undefined function: Com_PrintLogfile"
#endif

#ifndef __QCOMMON_H__
qboolean Com_IsDeveloper( void ){ return qtrue; }
#pragma message "Undefined function: Com_IsDeveloper"
#endif

cvar_t* com_logrcon;

//============================================================================
static char	*rd_buffer;
static int	rd_buffersize;
static void	(*rd_flush)( char *buffer, qboolean );

typedef enum {
	SSRT_LOG_DEBUG,
	SSRT_LOG_INFO,
	SSRT_LOG_WARN,
	SSRT_LOG_ERROR
} ssrtLogLevel_t;

static const char *SSRT_LogLevelName( ssrtLogLevel_t level )
{
	switch ( level ) {
	case SSRT_LOG_DEBUG:
		return "DEBUG";
	case SSRT_LOG_WARN:
		return "WARN";
	case SSRT_LOG_ERROR:
		return "ERROR";
	case SSRT_LOG_INFO:
	default:
		return "INFO";
	}
}

static ssrtLogLevel_t SSRT_LogLevelForType( msgtype_t type )
{
	switch ( type ) {
	case MSG_DEBUG:
	case MSG_DEBUG_NORDPRINT:
		return SSRT_LOG_DEBUG;
	case MSG_WARNING:
	case MSG_WARNING_NORDPRINT:
		return SSRT_LOG_WARN;
	case MSG_ERROR:
		return SSRT_LOG_ERROR;
	case MSG_DEFAULT:
	case MSG_NA:
	case MSG_NORDPRINT:
	default:
		return SSRT_LOG_INFO;
	}
}

static ssrtLogLevel_t SSRT_LogLevelForMessage( conChannel_t channel, msgtype_t type )
{
	if ( channel == CON_CHANNEL_ERROR ) {
		return SSRT_LOG_ERROR;
	}
	return SSRT_LogLevelForType( type );
}

static ssrtLogLevel_t SSRT_MinLogLevel( void )
{
	if ( !ssrt_logLevel || !ssrt_logLevel->string ) {
		return SSRT_LOG_DEBUG;
	}
	if ( !Q_stricmp( ssrt_logLevel->string, "error" ) ) {
		return SSRT_LOG_ERROR;
	}
	if ( !Q_stricmp( ssrt_logLevel->string, "warn" ) ) {
		return SSRT_LOG_WARN;
	}
	if ( !Q_stricmp( ssrt_logLevel->string, "info" ) ) {
		return SSRT_LOG_INFO;
	}
	return SSRT_LOG_DEBUG;
}

static qboolean SSRT_ShouldRedirect( msgtype_t type )
{
	return type != MSG_NORDPRINT && type != MSG_WARNING_NORDPRINT && type != MSG_DEBUG_NORDPRINT;
}

static const char *SSRT_ComponentForChannel( conChannel_t channel )
{
	switch ( channel ) {
	default:
		return "legacy";
	}
}

static void SSRT_FormatTimestamp( char *buffer, int bufferSize )
{
	time_t now;
	struct tm utcTime;
	struct tm *timeInfo;

	if ( bufferSize <= 0 ) {
		return;
	}
	buffer[0] = '\0';

	time( &now );
#ifdef _WIN32
	timeInfo = gmtime( &now );
	if ( !timeInfo ) {
		return;
	}
	utcTime = *timeInfo;
#else
	timeInfo = gmtime_r( &now, &utcTime );
	if ( !timeInfo ) {
		return;
	}
#endif

	strftime( buffer, bufferSize, "%Y-%m-%dT%H:%M:%SZ", &utcTime );
}

static void SSRT_AppendLogLine( char *out, int outSize, const char *prefix, const char *line, int lineLen )
{
	char trimmedLine[MAXPRINTMSG];

	if ( outSize <= 0 || lineLen < 0 ) {
		return;
	}

	while ( lineLen > 0 && line[lineLen - 1] == '\r' ) {
		lineLen--;
	}
	if ( lineLen >= (int)sizeof( trimmedLine ) ) {
		lineLen = sizeof( trimmedLine ) - 1;
	}
	memcpy( trimmedLine, line, lineLen );
	trimmedLine[lineLen] = '\0';

	Q_strncat( out, outSize, prefix );
	Q_strncat( out, outSize, trimmedLine );
	Q_strncat( out, outSize, "\n" );
}

static qboolean SSRT_Log( ssrtLogLevel_t level, const char *component, const char *msg, char *out, int outSize )
{
	char clean[MAXPRINTMSG];
	char prefix[128];
	char timestamp[32];
	const char *cursor;
	const char *newline;
	int len;

	if ( level < SSRT_MinLogLevel() ) {
		return qfalse;
	}

	if ( !msg ) {
		msg = "";
	}

	Q_strncpyz( clean, msg, sizeof( clean ) );
	len = strlen( clean );
	while ( len > 0 && ( clean[len - 1] == '\n' || clean[len - 1] == '\r' ) ) {
		clean[--len] = '\0';
	}

	if ( ssrt_logTimestamps && ssrt_logTimestamps->boolean ) {
		SSRT_FormatTimestamp( timestamp, sizeof( timestamp ) );
		if ( timestamp[0] ) {
			Com_sprintf( prefix, sizeof( prefix ), "%s %s %s ", timestamp, SSRT_LogLevelName( level ), component );
		} else {
			Com_sprintf( prefix, sizeof( prefix ), "%s %s ", SSRT_LogLevelName( level ), component );
		}
	} else {
		Com_sprintf( prefix, sizeof( prefix ), "%s %s ", SSRT_LogLevelName( level ), component );
	}

	out[0] = '\0';
	cursor = clean;
	do {
		newline = strchr( cursor, '\n' );
		if ( newline ) {
			SSRT_AppendLogLine( out, outSize, prefix, cursor, newline - cursor );
			cursor = newline + 1;
		} else {
			SSRT_AppendLogLine( out, outSize, prefix, cursor, strlen( cursor ) );
		}
	} while ( newline );

	return qtrue;
}

static qboolean SSRT_PrepareLogMessage( conChannel_t channel, char *msg, msgtype_t type, char *formatted, int formattedSize, char **outmsg )
{
	if ( !msg ) {
		msg = "";
	}

	if ( !ssrt_logStructured || !ssrt_logStructured->boolean ) {
		*outmsg = msg;
		return qtrue;
	}

	if ( !SSRT_Log( SSRT_LogLevelForMessage( channel, type ), SSRT_ComponentForChannel( channel ), msg, formatted, formattedSize ) ) {
		return qfalse;
	}

	*outmsg = formatted;
	return qtrue;
}


void Com_BeginRedirect (char *buffer, int buffersize, void (*flush)( char *, qboolean) )
{
	if (!buffer || !buffersize || !flush)
		return;
	rd_buffer = buffer;
	rd_buffersize = buffersize;
	rd_flush = flush;

	*rd_buffer = 0;
}

void Com_EndRedirect (void)
{
	if ( rd_flush ) {
		rd_flush(rd_buffer, qtrue);
	}

	rd_buffer = NULL;
	rd_buffersize = 0;
	rd_flush = NULL;
}

void Com_StopRedirect (void)
{
	rd_flush = NULL;
}

__cdecl void Com_PrintMessage( conChannel_t channel, char *msg, msgtype_t type) {

	//secures calls to Com_PrintMessage from recursion while redirect printing
	static qboolean lock = qfalse;
	char formatted[MAXPRINTMSG * 2];
	char *outmsg;
	int msglen;

	if(channel == CON_CHANNEL_LOGFILEONLY)
	{
		if ( SSRT_PrepareLogMessage( channel, msg, type, formatted, sizeof( formatted ), &outmsg ) ) {
			Com_PrintLogfile( outmsg );
		}
		return;
	}

	if ( !msg ) {
		msg = "";
	}
	msglen = strlen(msg);

	if(SSRT_ShouldRedirect(type) && !lock)
	{
	
		Sys_EnterCriticalSection(CRITSECT_RD_BUFFER);

		if ( !lock) {

			lock = qtrue;
			Com_PrintRedirect(msg, msglen);
			lock = qfalse;

			if ( rd_buffer && rd_flush) {
				if ((msglen + strlen(rd_buffer)) > (rd_buffersize - 1)) {

					lock = qtrue;
					rd_flush(rd_buffer, qfalse);
					lock = qfalse;

					*rd_buffer = 0;
				}
				Q_strncat(rd_buffer, rd_buffersize, msg);
				// TTimo nooo .. that would defeat the purpose
				//rd_flush(rd_buffer);
				//*rd_buffer = 0;
				if(!com_logrcon->boolean)
				{
					Sys_LeaveCriticalSection(CRITSECT_RD_BUFFER);
					return;
				}
			}
		}
		
		Sys_LeaveCriticalSection(CRITSECT_RD_BUFFER);
	
	}

	if ( !SSRT_PrepareLogMessage( channel, msg, type, formatted, sizeof( formatted ), &outmsg ) ) {
		return;
	}
	
	// echo to dedicated console and early console
	Sys_Print( outmsg );

	// logfile
	Com_PrintLogfile( outmsg );

}

/*
=============
Com_Printf

Both client and server can use this, and it will output
to the apropriate place.

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
=============
*/
void QDECL Com_Printf( conChannel_t channel, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

        Com_PrintMessage( channel, msg, MSG_DEFAULT);
}


/*
=============
Com_PrintfNoRedirect

This will not print to rcon

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
=============
*/
void QDECL Com_PrintNoRedirect( conChannel_t channel, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

        Com_PrintMessage( channel, msg, MSG_NORDPRINT);
}


/*
=============
Com_PrintWarning

Server can use this, and it will output
to the apropriate place.

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
=============
*/
void QDECL Com_PrintWarning( conChannel_t channel, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	memcpy(msg,"^3Warning: ", 12);

	va_start (argptr,fmt);
	Q_vsnprintf (&msg[11], (sizeof(msg)-12), fmt, argptr);
	va_end (argptr);

        Com_PrintMessage( channel, msg, MSG_WARNING);
}

void QDECL Com_DPrintWarning( conChannel_t channel, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if (!Com_IsDeveloper()) {
		return;
	}

	memcpy(msg, "^3Warning: ", 12);

	va_start(argptr, fmt);
	Q_vsnprintf (&msg[11], (sizeof(msg)-12), fmt, argptr);
	va_end(argptr);

    Com_PrintMessage(channel, msg, MSG_WARNING);
}

/*
=============
Com_PrintWarningNoRedirect

Server can use this, and it will output
to the apropriate place.

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
=============
*/
void QDECL Com_PrintWarningNoRedirect( conChannel_t channel, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	memcpy(msg,"^3Warning: ", 12);

	va_start (argptr,fmt);
	Q_vsnprintf (&msg[11], (sizeof(msg)-12), fmt, argptr);
	va_end (argptr);

        Com_PrintMessage( channel, msg, MSG_WARNING_NORDPRINT);
}


/*
=============
Com_PrintError

Server can use this, and it will output
to the apropriate place.

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
=============
*/
void QDECL Com_PrintError( conChannel_t channel, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	memcpy(msg,"^1Error: ", 10);

	va_start (argptr,fmt);
	Q_vsnprintf (&msg[9], (sizeof(msg)-10), fmt, argptr);
	va_end (argptr);

        Com_PrintMessage( channel, msg, MSG_ERROR);
}

/*
================
Com_DPrintf

A Com_Printf that only shows up if the "developer" cvar is set
================
*/
void QDECL Com_DPrintf( conChannel_t channel, const char *fmt, ...) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];
		
	if ( !Com_IsDeveloper() ) {
		return;			// don't confuse non-developers with techie stuff...
	}
	
	msg[0] = '^';
	msg[1] = '2';

	va_start (argptr,fmt);	
	Q_vsnprintf (&msg[2], (sizeof(msg)-3), fmt, argptr);
	va_end (argptr);

        Com_PrintMessage( channel, msg, MSG_DEBUG);
}



/*
================
Com_DPrintNoRedirect

A Com_Printf that only shows up if the "developer" cvar is set
This will not print to rcon
================
*/
void QDECL Com_DPrintNoRedirect( conChannel_t channel, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if ( !Com_IsDeveloper() ) {
		return;			// don't confuse non-developers with techie stuff...
	}
	
	msg[0] = '^';
	msg[1] = '2';

	va_start (argptr,fmt);	
	Q_vsnprintf (&msg[2], (sizeof(msg)-3), fmt, argptr);
	va_end (argptr);

        Com_PrintMessage( channel, msg, MSG_DEBUG_NORDPRINT);
}

void QDECL Com_PrintScriptRuntimeWarning(const char *fmt, ...)
{

	va_list		argptr;
	char		msg[MAXPRINTMSG];
	char		finalmsg[MAXPRINTMSG];

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Com_sprintf(finalmsg, sizeof(finalmsg), "^6Script Runtime Warning: %s\n", msg);

        Com_PrintMessage( CON_CHANNEL_SCRIPT, finalmsg, MSG_WARNING);
}



#define MAX_REDIRECTDESTINATIONS 4

static void (*rd_destinations[MAX_REDIRECTDESTINATIONS])( const char *buffer, int len );


void Com_PrintRedirect(char* msg, int msglen)
{

    int i;

    for(i = 0; i < MAX_REDIRECTDESTINATIONS; i++)
    {
        if(rd_destinations[i] == NULL)
            return;

        rd_destinations[i](msg, msglen);

    }

}


void Com_AddRedirect(void (*rd_dest)(const char *, int))
{
    int i;

    for(i = 0; i < MAX_REDIRECTDESTINATIONS; i++)
    {
        if(rd_destinations[i] == rd_dest)
        {
            Com_Error(ERR_FATAL, "Com_AddRedirect: Attempt to add an already defined redirect function twice.");
            return;
        }

        if(rd_destinations[i] == NULL)
        {
            rd_destinations[i] = rd_dest;
            return;
        }
    }
    Com_Error(ERR_FATAL, "Com_AddRedirect: Out of redirect handles. Increase MAX_REDIRECTDESTINATIONS to add more redirect destinations");
}
