/*
 * Copyright (c) 1996 Regents of the University of Michigan.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of Michigan at Ann Arbor. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 */

/*
 * ldap_op.c - routines to perform LDAP operations
 */

#include "portable.h"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>

#ifdef KERBEROS
#include <krb.h>
#endif /* KERBEROS */

#include <lber.h>
#include <ldap.h>

#include "portable.h"
#include "slurp.h"

/* Forward references */
static int get_changetype( char * );
static struct berval **make_singlevalued_berval( char	*, int );
static int op_ldap_add( Ri *, Re *, char ** );
static int op_ldap_modify( Ri *, Re *, char ** );
static int op_ldap_delete( Ri *, Re *, char ** );
static int op_ldap_modrdn( Ri *, Re *, char ** );
static LDAPMod *alloc_ldapmod();
static void free_ldapmod( LDAPMod * );
static void free_ldmarr( LDAPMod ** );
static int getmodtype( char * );
static void dump_ldm_array( LDAPMod ** );
static char **read_krbnames( Ri * );
static void upcase( char * );
static int do_bind( Ri *, int * );
static int do_unbind( Ri * );


/* External references */
#ifdef DECL_SYS_ERRLIST
extern char *sys_errlist[];
#endif /* DECL_SYS_ERRLIST */

extern char *ch_malloc( unsigned long );

static char *kattrs[] = {"kerberosName", NULL };
static struct timeval kst = {30L, 0L};



/*
 * Determine the type of ldap operation being performed and call the
 * appropriate routine.
 * - If successful, returns ERR_DO_LDAP_OK
 * - If a retryable error occurs, ERR_DO_LDAP_RETRYABLE is returned.
 *   The caller should wait a while and retry the operation.
 * - If a fatal error occurs, ERR_DO_LDAP_FATAL is returned.  The caller
 *   should reject the operation and continue with the next replication
 *   entry.
 */
int
do_ldap(
    Ri		*ri,
    Re		*re,
    char	**errmsg
)
{
    int	rc = 0;
    int	lderr = LDAP_SUCCESS;
    int	retry = 2;
    char *msg;

    *errmsg = NULL;

    while ( retry > 0 ) {
	if ( ri->ri_ldp == NULL ) {
	    rc = do_bind( ri, &lderr );
	    if ( rc != BIND_OK ) {
		return DO_LDAP_ERR_RETRYABLE;
	    }
	}

	switch ( re->re_changetype ) {
	case T_ADDCT:
	    lderr = op_ldap_add( ri, re, errmsg );
	    if ( lderr != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_ANY,
			"Error: ldap_add_s failed adding \"%s\": %s\n",
			*errmsg ? *errmsg : ldap_err2string( lderr ),
			re->re_dn, 0 );
	    }
	    break;
	case T_MODIFYCT:
	    lderr = op_ldap_modify( ri, re, errmsg );
	    if ( lderr != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_ANY,
			"Error: ldap_modify_s failed modifying \"%s\": %s\n",
			*errmsg ? *errmsg : ldap_err2string( lderr ),
			re->re_dn, 0 );
	    }
	    break;
	case T_DELETECT:
	    lderr = op_ldap_delete( ri, re, errmsg );
	    if ( lderr != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_ANY,
			"Error: ldap_delete_s failed deleting \"%s\": %s\n",
			*errmsg ? *errmsg : ldap_err2string( lderr ),
			re->re_dn, 0 );
	    }
	    break;
	case T_MODRDNCT:
	    lderr = op_ldap_modrdn( ri, re, errmsg );
	    if ( lderr != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_ANY,
			"Error: ldap_modrdn_s failed modifying %s: %s\n",
			*errmsg ? *errmsg : ldap_err2string( lderr ),
			re->re_dn, 0 );
	    }
	    break;
	default:
	    Debug( LDAP_DEBUG_ANY,
		    "Error: do_ldap: bad op \"%d\", dn = \"%s\"\n",
		    re->re_changetype, re->re_dn, 0 );
	    return DO_LDAP_ERR_FATAL;
	}

	/*
	 * Analyze return code.  If ok, just return.  If LDAP_SERVER_DOWN,
	 * we may have been idle long enough that the remote slapd timed
	 * us out.  Rebind and try again.
	 */
	if ( lderr == LDAP_SUCCESS ) {
	    return DO_LDAP_OK;
	} else if ( lderr == LDAP_SERVER_DOWN ) {
	    /* The LDAP server may have timed us out - rebind and try again */
	    (void) do_unbind( ri );
	    retry--;
	} else {
	    return DO_LDAP_ERR_FATAL;
	}
    }
    return DO_LDAP_ERR_FATAL;
}




/*
 * Perform an ldap add operation.
 */
static int
op_ldap_add(
    Ri		*ri,
    Re		*re,
    char	**errmsg
)
{
    Mi		*mi;
    int		nattrs, rc = 0, i;
    LDAPMod	*ldm, **ldmarr;
    int		lderr = 0;

    nattrs = i = 0;
    ldmarr = NULL;

    /*
     * Construct a null-terminated array of LDAPMod structs.
     */
    mi = re->re_mods;
    while ( mi[ i ].mi_type != NULL ) {
	ldm = alloc_ldapmod();
	ldmarr = ( LDAPMod ** ) ch_realloc( ldmarr,
		( nattrs + 2 ) * sizeof( LDAPMod * ));
	ldmarr[ nattrs ] = ldm;
	ldm->mod_op = LDAP_MOD_BVALUES;
	ldm->mod_type = mi[ i ].mi_type;
	ldm->mod_bvalues =
		make_singlevalued_berval( mi[ i ].mi_val, mi[ i ].mi_len );
	i++;
	nattrs++;
    }

    if ( ldmarr != NULL ) {
	ldmarr[ nattrs ] = NULL;

	/* Perform the operation */
	Debug( LDAP_DEBUG_ARGS, "replica %s:%d - add dn \"%s\"\n",
		ri->ri_hostname, ri->ri_port, re->re_dn );
	rc = ldap_add_s( ri->ri_ldp, re->re_dn, ldmarr );
	lderr = ri->ri_ldp->ld_errno;
    } else {
	*errmsg = "No modifications to do";
	Debug( LDAP_DEBUG_ANY,
		"Error: op_ldap_add: no mods to do (%s)!", re->re_dn, 0, 0 );
    }
    free_ldmarr( ldmarr );
    return( lderr ); 
}




/*
 * Perform an ldap modify operation.
 */
#define	AWAITING_OP -1
static int
op_ldap_modify(
    Ri		*ri,
    Re		*re,
    char	**errmsg
)
{
    Mi		*mi;
    int		state;	/* This code is a simple-minded state machine */
    int		nvals;	/* Number of values we're modifying */
    int		nops;	/* Number of LDAPMod structs in ldmarr */
    LDAPMod	*ldm, *nldm, **ldmarr;
    int		i, len;
    char	*type, *value;
    int		rc = 0;

    state = AWAITING_OP;
    nvals = 0;
    nops = 0;
    ldmarr = NULL;

    if ( re->re_mods == NULL ) {
	*errmsg = "No arguments given";
	Debug( LDAP_DEBUG_ANY, "Error: op_ldap_modify: no arguments\n",
		0, 0, 0 );
	    return -1;
    }

    /*
     * Construct a null-terminated array of LDAPMod structs.
     */
    for ( mi = re->re_mods, i = 0; mi[ i ].mi_type != NULL; i++ ) {
	type = mi[ i ].mi_type;
	value = mi[ i ].mi_val;
	len = mi[ i ].mi_len;
	switch ( getmodtype( type )) {
	case T_MODSEP:
	    state = T_MODSEP; /* Got a separator line "-\n" */
	    continue;
	case T_MODOPADD:
	    state = T_MODOPADD;
	    ldmarr = ( LDAPMod ** )
		    ch_realloc(ldmarr, (( nops + 2 ) * ( sizeof( LDAPMod * ))));
	    ldmarr[ nops ] = ldm = alloc_ldapmod();
	    ldm->mod_op = LDAP_MOD_ADD | LDAP_MOD_BVALUES;
	    ldm->mod_type = value;
	    nvals = 0;
	    nops++;
	    break;
	case T_MODOPREPLACE:
	    state = T_MODOPREPLACE;
	    ldmarr = ( LDAPMod ** )
		    ch_realloc(ldmarr, (( nops + 2 ) * ( sizeof( LDAPMod * ))));
	    ldmarr[ nops ] = ldm = alloc_ldapmod();
	    ldm->mod_op = LDAP_MOD_REPLACE | LDAP_MOD_BVALUES;
	    ldm->mod_type = value;
	    nvals = 0;
	    nops++;
	    break;
	case T_MODOPDELETE:
	    state = T_MODOPDELETE;
	    ldmarr = ( LDAPMod ** )
		    ch_realloc(ldmarr, (( nops + 2 ) * ( sizeof( LDAPMod * ))));
	    ldmarr[ nops ] = ldm = alloc_ldapmod();
	    ldm->mod_op = LDAP_MOD_DELETE | LDAP_MOD_BVALUES;
	    ldm->mod_type = value;
	    nvals = 0;
	    nops++;
	    break;
	default:
	    if ( state == AWAITING_OP ) {
		Debug( LDAP_DEBUG_ANY,
			"Error: op_ldap_modify: unknown mod type \"%s\"\n",
			type, 0, 0 );
		continue;
	    }

	    /*
	     * We should have an attribute: value pair here.
	     * Construct the mod_bvalues part of the ldapmod struct.
	     */
	    if ( strcasecmp( type, ldm->mod_type )) {
		Debug( LDAP_DEBUG_ANY,
			"Error: malformed modify op, %s: %s (expecting %s:)\n",
			type, value, ldm->mod_type );
		continue;
	    }
	    ldm->mod_bvalues = ( struct berval ** )
		    ch_realloc( ldm->mod_bvalues,
		    ( nvals + 2 ) * sizeof( struct berval * ));
	    ldm->mod_bvalues[ nvals + 1 ] = NULL;
	    ldm->mod_bvalues[ nvals ] = ( struct berval * )
		    ch_malloc( sizeof( struct berval ));
	    ldm->mod_bvalues[ nvals ]->bv_val = value;
	    ldm->mod_bvalues[ nvals ]->bv_len = len;
	    nvals++;
	}
    }
    ldmarr[ nops ] = NULL;

    if ( nops > 0 ) {
	/* Actually perform the LDAP operation */
	Debug( LDAP_DEBUG_ARGS, "replica %s:%d - modify dn \"%s\"\n",
		ri->ri_hostname, ri->ri_port, re->re_dn );
	rc = ldap_modify_s( ri->ri_ldp, re->re_dn, ldmarr );
    }
    free_ldmarr( ldmarr );
    return( rc );
}




/*
 * Perform an ldap delete operation.
 */
static int
op_ldap_delete(
    Ri		*ri,
    Re		*re,
    char	**errmsg
)
{
    int		rc;

    Debug( LDAP_DEBUG_ARGS, "replica %s:%d - delete dn \"%s\"\n",
	    ri->ri_hostname, ri->ri_port, re->re_dn );
    rc = ldap_delete_s( ri->ri_ldp, re->re_dn );

    return( rc );
}




/*
 * Perform an ldap modrdn operation.
 */
#define	GOT_NEWRDN		1
#define	GOT_DRDNFLAGSTR		2
#define	GOT_ALLNEWRDNFLAGS	( GOT_NEWRDN | GOT_DRDNFLAGSTR )
static int
op_ldap_modrdn(
    Ri		*ri,
    Re		*re,
    char	**errmsg
)
{
    int		rc = 0;
    Mi		*mi;
    int		i;
    int		state = 0;
    int		drdnflag = -1;
    char	*newrdn;

    if ( re->re_mods == NULL ) {
	*errmsg = "No arguments given";
	Debug( LDAP_DEBUG_ANY, "Error: op_ldap_modrdn: no arguments\n",
		0, 0, 0 );
	    return -1;
    }

    /*
     * Get the arguments: should see newrdn: and deleteoldrdn: args.
     */
    for ( mi = re->re_mods, i = 0; mi[ i ].mi_type != NULL; i++ ) {
	if ( !strcmp( mi[ i ].mi_type, T_NEWRDNSTR )) {
	    newrdn = mi[ i ].mi_val;
	    state |= GOT_NEWRDN;
	} else if ( !strcmp( mi[ i ].mi_type, T_DRDNFLAGSTR )) {
	    state |= GOT_DRDNFLAGSTR;
	    if ( !strcmp( mi[ i ].mi_val, "0" )) {
		drdnflag = 0;
	    } else if ( !strcmp( mi[ i ].mi_val, "1" )) {
		drdnflag = 1;
	    } else {
		Debug( LDAP_DEBUG_ANY,
			"Error: op_ldap_modrdn: bad deleteoldrdn arg \"%s\"\n",
			mi[ i ].mi_val, 0, 0 );
		*errmsg = "Incorrect argument to deleteoldrdn";
		return -1;
	    }
	} else {
	    Debug( LDAP_DEBUG_ANY, "Error: op_ldap_modrdn: bad type \"%s\"\n",
		    mi[ i ].mi_type, 0, 0 );
	    *errmsg = "Bad value in replication log entry";
	    return -1;
	}
    }

    /*
     * Punt if we don't have all the args.
     */
    if ( state != GOT_ALLNEWRDNFLAGS ) {
	Debug( LDAP_DEBUG_ANY, "Error: op_ldap_modrdn: missing arguments\n",
		0, 0, 0 );
	*errmsg = "Missing argument: requires \"newrdn\" and \"deleteoldrdn\"";
	return -1;
    }

#ifdef LDAP_DEBUG
    if ( ldap_debug & LDAP_DEBUG_ARGS ) {
	char buf[ 256 ];
	char *buf2;
	sprintf( buf, "%s:%d", ri->ri_hostname, ri->ri_port );
	buf2 = (char *) ch_malloc( strlen( re->re_dn ) + strlen( mi->mi_val )
		+ 10 );
	sprintf( buf2, "(\"%s\" -> \"%s\")", re->re_dn, mi->mi_val );
	Debug( LDAP_DEBUG_ARGS,
		"replica %s - modify rdn %s (flag: %d)\n",
		buf, buf2, drdnflag );
	free( buf2 );
    }
#endif /* LDAP_DEBUG */

    /* Do the modrdn */
    rc = ldap_modrdn2_s( ri->ri_ldp, re->re_dn, mi->mi_val, drdnflag );

    return( ri->ri_ldp->ld_errno );
}



/*
 * Allocate and initialize an ldapmod struct.
 */
static LDAPMod *
alloc_ldapmod()
{
    LDAPMod	*ldm;

    ldm = ( struct ldapmod * ) ch_malloc( sizeof ( struct ldapmod ));
    ldm->mod_type = NULL;
    ldm->mod_bvalues = ( struct berval ** ) NULL;
    return( ldm );
}



/*
 * Free an ldapmod struct associated mod_bvalues.  NOTE - it is assumed
 * that mod_bvalues and mod_type contain pointers to the same block of memory
 * pointed to by the repl struct.  Therefore, it's not freed here.
 */
static void
free_ldapmod(
LDAPMod *ldm )
{
    int		i;

    if ( ldm == NULL ) {
	return;
    }
    if ( ldm->mod_bvalues != NULL ) {
	for ( i = 0; ldm->mod_bvalues[ i ] != NULL; i++ ) {
	    free( ldm->mod_bvalues[ i ] );
	}
	free( ldm->mod_bvalues );
    }
    free( ldm );
    return;
}


/*
 * Free an an array of LDAPMod pointers and the LDAPMod structs they point
 * to.
 */
static void
free_ldmarr(
LDAPMod **ldmarr )
{
    int	i;

    for ( i = 0; ldmarr[ i ] != NULL; i++ ) {
	free_ldapmod( ldmarr[ i ] );
    }
    free( ldmarr );
}


/*
 * Create a berval with a single value. 
 */
static struct berval **
make_singlevalued_berval( 
char	*value,
int	len )
{
    struct berval **p;

    p = ( struct berval ** ) ch_malloc( 2 * sizeof( struct berval * ));
    p[ 0 ] = ( struct berval * ) ch_malloc( sizeof( struct berval ));
    p[ 1 ] = NULL;
    p[ 0 ]->bv_val = value;
    p[ 0 ]->bv_len = len;
    return( p );
}


/*
 * Given a modification type (string), return an enumerated type.
 * Avoids ugly copy in op_ldap_modify - lets us use a switch statement
 * there.
 */
static int
getmodtype( 
char *type )
{
    if ( !strcmp( type, T_MODSEPSTR )) {
	return( T_MODSEP );
    }
    if ( !strcmp( type, T_MODOPADDSTR )) {
	return( T_MODOPADD );
    }
    if ( !strcmp( type, T_MODOPREPLACESTR )) {
	return( T_MODOPREPLACE );
    }
    if ( !strcmp( type, T_MODOPDELETESTR )) {
	return( T_MODOPDELETE );
    }
    return( T_ERR );
}


/*
 * Perform an LDAP unbind operation.  If replica is NULL, or the
 * repl_ldp is NULL, just return LDAP_SUCCESS.  Otherwise, unbind,
 * set the ldp to NULL, and return the result of the unbind call.
 */
static int
do_unbind(
    Ri	*ri
)
{
    int		rc = LDAP_SUCCESS;

    if (( ri != NULL ) && ( ri->ri_ldp != NULL )) {
	rc = ldap_unbind( ri->ri_ldp );
	if ( rc != LDAP_SUCCESS ) {
	    Debug( LDAP_DEBUG_ANY,
		    "Error: do_unbind: ldap_unbind failed for %s:%d: %s\n",
		    ldap_err2string( rc ), ri->ri_hostname, ri->ri_port );
	}
	ri->ri_ldp = NULL;
    }
    return rc;
}



/*
 * Perform an LDAP bind operation to the replication site given
 * by replica.  If replica->repl_ldp is non-NULL, then we unbind
 * from the replica before rebinding.  It should be safe to call
 * this to re-connect if the replica's connection goes away
 * for some reason.
 *
 * Returns 0 on success, -1 if an LDAP error occurred, and a return
 * code > 0 if some other error occurred, e.g. invalid bind method.
 * If an LDAP error occurs, the LDAP error is returned in lderr.
 */
static int
do_bind( 
    Ri	*ri,
    int	*lderr
)
{
    int		rc;
    int		ldrc;
    char	msgbuf[ 1024];
#ifdef KERBEROS
    int retval = 0;
    int kni, got_tgt;
    char **krbnames;
    char *skrbnames[ 2 ];
    char realm[ REALM_SZ ];
    char name[ ANAME_SZ ];
    char instance[ INST_SZ ];
#endif /* KERBEROS */

    *lderr = 0;

    if ( ri == NULL ) {
	Debug( LDAP_DEBUG_ANY, "Error: do_bind: null ri ptr\n", 0, 0, 0 );
	return( BIND_ERR_BADRI );
    }

    if ( ri->ri_ldp != NULL ) {
	ldrc = ldap_unbind( ri->ri_ldp );
	if ( ldrc != LDAP_SUCCESS ) {
	    Debug( LDAP_DEBUG_ANY,
		    "Error: do_bind: ldap_unbind failed: %s\n",
		    ldap_err2string( ldrc ), 0, 0 );
	}
	ri->ri_ldp = NULL;
    }

    Debug( LDAP_DEBUG_ARGS, "Open connection to %s:%d\n",
	    ri->ri_hostname, ri->ri_port, 0 );
    ri->ri_ldp = ldap_open( ri->ri_hostname, ri->ri_port );
    if ( ri->ri_ldp == NULL ) {
	Debug( LDAP_DEBUG_ANY, "Error: ldap_open(%s, %d) failed: %s\n",
		ri->ri_hostname, ri->ri_port, sys_errlist[ errno ] );
	return( BIND_ERR_OPEN );
    }

    /*
     * Set ldap library options to (1) not follow referrals, and 
     * (2) restart the select() system call.
     */
#ifdef LDAP_REFERRALS
    ri->ri_ldp->ld_options &= ~LDAP_OPT_REFERRALS;
#endif /* LDAP_REFERRALS */
    ri->ri_ldp->ld_options |= LDAP_OPT_RESTART;

    switch ( ri->ri_bind_method ) {
    case AUTH_KERBEROS:
#ifndef KERBEROS
	Debug( LDAP_DEBUG_ANY,
	    "Error: Kerberos bind for %s:%d, but not compiled w/kerberos\n",
	    ri->ri_hostname, ri->ri_port, 0 );
	return( BIND_ERR_KERBEROS_FAILED );
#else /* KERBEROS */
	/*
	 * Bind using kerberos.
	 * If "bindprincipal" was given in the config file, then attempt
	 * to get a TGT for that principal (via the srvtab file).  If only
	 * a binddn was given, then we need to read that entry to get
	 * the kerberosName attributes, and try to get a TGT for one
	 * of them.  All are tried.  The first one which succeeds is
	 * returned.  XXX It might be a good idea to just require a
	 * bindprincipal.  Reading the entry every time might be a significant
	 * amount of overhead, if the connection is closed between most
	 * updates.
	 */

	if ( ri->ri_principal != NULL ) {
	    skrbnames[ 0 ] = ri->ri_principal;
	    skrbnames[ 1 ] = NULL;
	    krbnames = skrbnames;
	} else {
	    krbnames = read_krbnames( ri );
	}	
	    
	if (( krbnames == NULL ) || ( krbnames[ 0 ] == NULL )) {
	    Debug( LDAP_DEBUG_ANY,
		    "Error: Can't find krbname for binddn \"%s\"\n",
		    ri->ri_bind_dn, 0, 0 );
	    retval = BIND_ERR_KERBEROS_FAILED;
	    goto kexit;
	}
	/*
	 * Now we've got one or more kerberos principals.  See if any
	 * of them are in the srvtab file.
	 */
	got_tgt = 0;
	for ( kni = 0; krbnames[ kni ] != NULL; kni++ ) {
	    rc = kname_parse( name, instance, realm, krbnames[ kni ]);
	    if ( rc != KSUCCESS ) {
		continue;
	    }
	    upcase( realm );
	    rc = krb_get_svc_in_tkt( name, instance, realm, "krbtgt", realm,
		    1, ri->ri_srvtab );
	    if ( rc != KSUCCESS) {
		Debug( LDAP_DEBUG_ANY, "Error: Can't get TGT for %s: %s\n",
			krbnames[ kni ], krb_err_txt[ rc ], 0 );
	    } else {
		got_tgt = 1;
		break;
	    }
	}
	if (!got_tgt) {
	    Debug( LDAP_DEBUG_ANY,
		    "Error: Could not obtain TGT for DN \"%s\"\n", 
		    ri->ri_bind_dn, 0, 0 );
	    retval = BIND_ERR_KERBEROS_FAILED;
	    goto kexit;
	}
	/*
	 * We've got a TGT.  Do a kerberos bind.
	 */
	Debug( LDAP_DEBUG_ARGS, "bind to %s:%d as %s (kerberos)\n",
		ri->ri_hostname, ri->ri_port, ri->ri_bind_dn );
	ldrc = ldap_kerberos_bind_s( ri->ri_ldp, ri->ri_bind_dn );
	ri->ri_principal = strdup( krbnames[ kni ] );
	if ( ldrc != LDAP_SUCCESS ) {
	    Debug( LDAP_DEBUG_ANY, "Error: kerberos bind for %s:%dfailed: %s\n",
		ri->ri_hostname, ri->ri_port, ldap_err2string( ldrc ));
	    *lderr = ldrc;
	    retval = BIND_ERR_KERBEROS_FAILED;
	    goto kexit;
	}
kexit:	if ( krbnames != NULL ) {
	    ldap_value_free( krbnames );
	}
	return( retval);
	break;
#endif /* KERBEROS */
    case AUTH_SIMPLE:
	/*
	 * Bind with a plaintext password.
	 */
	Debug( LDAP_DEBUG_ARGS, "bind to %s:%d as %s (simple)\n",
		ri->ri_hostname, ri->ri_port, ri->ri_bind_dn );
	ldrc = ldap_simple_bind_s( ri->ri_ldp, ri->ri_bind_dn,
		ri->ri_password );
	if ( ldrc != LDAP_SUCCESS ) {
	    Debug( LDAP_DEBUG_ANY,
		    "Error: ldap_simple_bind_s for %s:%d failed: %s\n",
		    ri->ri_hostname, ri->ri_port, ldap_err2string( ldrc ));
	    *lderr = ldrc;
	    return( BIND_ERR_SIMPLE_FAILED );
	} else {
	    return( BIND_OK );
	}
	break;
    default:
	Debug(  LDAP_DEBUG_ANY,
		"Error: do_bind: unknown auth type \"%d\" for %s:%d\n",
		ri->ri_bind_method, ri->ri_hostname, ri->ri_port );
	return( BIND_ERR_BAD_ATYPE );
    }
}





/*
 * For debugging.  Print the contents of an ldmarr array.
 */
static void
dump_ldm_array(
LDAPMod **ldmarr )
{
    int			 i, j;
    LDAPMod		*ldm;
    struct berval	*b;
    char		*msgbuf;

    for ( i = 0; ldmarr[ i ] != NULL; i++ ) {
	ldm = ldmarr[ i ];
	Debug( LDAP_DEBUG_TRACE,
		"Trace (%d): *** ldmarr[ %d ] contents:\n",
		getpid(), i, 0 );
	Debug( LDAP_DEBUG_TRACE,
		"Trace (%d): *** ldm->mod_op: %d\n",
		getpid(), ldm->mod_op, 0 );
	Debug( LDAP_DEBUG_TRACE,
		"Trace (%d): *** ldm->mod_type: %s\n",
		getpid(), ldm->mod_type, 0 );
	if ( ldm->mod_bvalues != NULL ) {
	    for ( j = 0; ( b = ldm->mod_bvalues[ j ] ) != NULL; j++ ) {
		msgbuf = ch_malloc( b->bv_len + 512 );
		sprintf( msgbuf, "***** bv[ %d ] len = %d, val = <%s>",
			j, b->bv_len, b->bv_val );
		Debug( LDAP_DEBUG_TRACE,
			"Trace (%d):%s\n", getpid(), msgbuf, 0 );
		free( msgbuf );
	    }
	}
    }
}


/*
 * Get the kerberos names from the binddn for "replica" via an ldap search.
 * Returns a null-terminated array of char *, or NULL if the entry could
 * not be found or there were no kerberosName attributes.  The caller is
 * responsible for freeing the returned array and strings it points to.
 */
static char **
read_krbnames(
    Ri	*ri
)
{
    int rc;
    char **krbnames;
    int ne;
    LDAPMessage *result, *entry;

    /* First need to bind as NULL */
    rc = ldap_simple_bind_s( ri->ri_ldp, NULL, NULL );
    if ( rc != LDAP_SUCCESS ) {
	Debug( LDAP_DEBUG_ANY,
		"Error: null bind failed getting krbnames for %s:%d: %s\n",
		ri->ri_hostname, ri->ri_port, ldap_err2string( rc ));
	return( NULL );
    }
    rc = ldap_search_st( ri->ri_ldp, ri->ri_bind_dn, LDAP_SCOPE_BASE,
	    "objectclass=*", kattrs, 0, &kst, &result );
    if ( rc != LDAP_SUCCESS ) {
	Debug( LDAP_DEBUG_ANY,
		"Error: search failed getting krbnames for %s:%d: %s\n",
		ri->ri_hostname, ri->ri_port, ldap_err2string( rc ));
	return( NULL );
    }
    ne = ldap_count_entries( ri->ri_ldp, result );
    if ( ne == 0 ) {
	Debug( LDAP_DEBUG_ANY,
		"Error: Can't find entry \"%s\" for %s:%d kerberos bind\n",
		ri->ri_bind_dn, ri->ri_hostname, ri->ri_port );
	    return( NULL );
    }
    if ( ne > 1 ) {
	Debug( LDAP_DEBUG_ANY,
		"Error: Kerberos binddn \"%s\" for %s:%dis ambiguous\n",
		ri->ri_bind_dn, ri->ri_hostname, ri->ri_port );
	    return( NULL );
    }
    entry = ldap_first_entry( ri->ri_ldp, result );
    if ( entry == NULL ) {
	Debug( LDAP_DEBUG_ANY,
		"Error: Can't find \"%s\" for kerberos binddn for %s:%d\n",
		    ri->ri_bind_dn, ri->ri_hostname, ri->ri_port );
	return( NULL );
    }
    krbnames = ldap_get_values( ri->ri_ldp, entry, "kerberosName" );
    ldap_msgfree( result );
    return( krbnames );
}



/*
 * upcase a string
 */
static void
upcase(
char *s )
{
    char *p;

    for ( p = s; ( p != NULL ) && ( *p != '\0' ); p++ ) {
	if ( islower( *p )) {
	    *p = toupper( *p );
	}
    }
}
