/*
 * Copyright (c) 1999 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * "Portions Copyright (c) 1999 Apple Computer, Inc.  All Rights
 * Reserved.  This file contains Original Code and/or Modifications of
 * Original Code as defined in and that are subject to the Apple Public
 * Source License Version 1.0 (the 'License').	You may not use this file
 * except in compliance with the License.  Please obtain a copy of the
 * License at http://www.apple.com/publicsource and read it before using
 * this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License."
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*-
 * Copyright (c) 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *	  must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#include <sys/param.h>
#include <sys/mount.h>
#include <sys/syslog.h>

#include <err.h>
#include <errno.h>
#include <fstab.h>
#include <stdlib.h>
#include <string.h>

#include "mntopts.h"

/*****************************************************************************/

int getmnt_silent = 1;

/*****************************************************************************/

int getmntopts(options, m0, flagp, altflagp)
	const char *options;
	const struct mntopt *m0;
	int *flagp;
	int *altflagp;
{
	const struct mntopt *m;
	int negative;
	char *opt,  *optbuf,  *p;
	int *thisflagp;
	
	/* Copy option string, since it is about to be torn asunder... */
	optbuf = malloc(strlen(options) + 1);
	if (!optbuf)
	{
		return (ENOMEM);
	}
	
	strcpy(optbuf, options);
	
	for (opt = optbuf; (opt = strtok(opt, ",")) != NULL; opt = NULL)
	{											/* Check for "no" prefix. */
		if (opt[0] == 'n' && opt[1] == 'o')
		{
			negative = 1;
			opt += 2;
		}
		else
		{
			negative = 0;
		}
		
		/*
		 * for options with assignments in them (ie. quotas)
		 * ignore the assignment as it's handled elsewhere
		 */
		p = strchr(opt, '=');
		if (p)
		{
			*p = '\0';
		}
		
		/* Scan option table. */
		for (m = m0; m->m_option != NULL; ++m)
		{
			if (strcasecmp(opt, m->m_option) == 0)
			{
				break;
			}
		}
		
		/* Save flag, or fail if option is not recognised. */
		if (m->m_option)
		{
			thisflagp = m->m_altloc ? altflagp : flagp;
			if (negative == m->m_inverse)
			{
				*thisflagp |= m->m_flag;
			}
			else
			{
				*thisflagp &= ~m->m_flag;
			}
		}
		else if (!getmnt_silent)
		{
			free(optbuf);
			syslog(LOG_ERR, "-o %s: option not supported", opt);
			return (EINVAL);
		}
		else
		{
			syslog(LOG_ERR, "-o %s: option ignored", opt);
		}
	}
	
	free(optbuf);
	
	return (0);
}

/*****************************************************************************/
