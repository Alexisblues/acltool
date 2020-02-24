/*
 * aclcmds.c - ACL commands
 *
 * Copyright (c) 2020, Peter Eriksson <pen@lysator.liu.se>
 *
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <ftw.h>

#include "acltool.h"


static CONFIG *w_cfgp = NULL;
static size_t w_c = 0;


int
str2style(const char *str,
	  ACL_STYLE *sp) {
  if (!str || !*str)
    return 0;
  
  if (strcasecmp(str, "default") == 0)
    *sp = ACL_STYLE_DEFAULT;
  else if (strcasecmp(str, "brief") == 0)
    *sp = ACL_STYLE_BRIEF;
  else if (strcasecmp(str, "verbose") == 0)
    *sp = ACL_STYLE_VERBOSE;
  else if (strcasecmp(str, "csv") == 0)
    *sp = ACL_STYLE_CSV;
  else if (strcasecmp(str, "samba") == 0)
    *sp = ACL_STYLE_SAMBA;
  else if (strcasecmp(str, "icacls") == 0)
    *sp = ACL_STYLE_ICACLS;
  else if (strcasecmp(str, "solaris") == 0)
    *sp = ACL_STYLE_SOLARIS;
  else if (strcasecmp(str, "primos") == 0)
    *sp = ACL_STYLE_PRIMOS;
  else
    return -1;

  return 1;
}

const char *
style2str(ACL_STYLE s) {
  switch (s) {
  case ACL_STYLE_DEFAULT:
    return "Default";
  case ACL_STYLE_BRIEF:
    return "Brief";
  case ACL_STYLE_VERBOSE:
    return "Verbose";
  case ACL_STYLE_CSV:
    return "CSV";
  case ACL_STYLE_SAMBA:
    return "Samba";
  case ACL_STYLE_ICACLS:
    return "ICACLS";
  case ACL_STYLE_SOLARIS:
    return "Solaris";
  case ACL_STYLE_PRIMOS:
    return "PRIMOS";
  }

  return NULL;
}

char *
mode2str(mode_t m) {
  static char buf[11];

  switch (m & S_IFMT) {
  case S_IFIFO:
    buf[0] = 'p';
    break;
  case S_IFCHR:
    buf[0] = 'c';
    break;
  case S_IFDIR:
    buf[0] = 'd';
    break;
  case S_IFBLK:
    buf[0] = 'b';
    break;
  case S_IFREG:
    buf[0] = '-';
    break;
  case S_IFLNK:
    buf[0] = 'l';
    break;
  case S_IFSOCK:
    buf[0] = 's';
    break;
#ifdef S_IFWHT
  case S_IFWHT:
    buf[0] = 'w';
    break;
#endif
  default:
    buf[0] = '?';
  }
  
  buf[1] = (m & S_IRUSR ? 'r' : '-');
  buf[2] = (m & S_IWUSR ? 'w' : '-');
  buf[3] = (m & S_IXUSR ? (m & S_ISUID ? 's' : 'x') : (m & S_ISUID ? 'S' : '-'));

  buf[4] = (m & S_IRGRP ? 'r' : '-');
  buf[5] = (m & S_IWGRP ? 'w' : '-');
  buf[6] = (m & S_IXGRP ? (m & S_ISGID ? 's' : 'x') : (m & S_ISGID ? 'S' : '-'));

  buf[7] = (m & S_IROTH ? 'r' : '-');
  buf[8] = (m & S_IWOTH ? 'w' : '-');
  buf[9] = (m & S_IXOTH ? (m & S_ISVTX ? 't' : 'x') : (m & S_ISVTX ? 'T' : '-'));

  buf[10] = '\0';
  return buf;
}


int
_acl_filter_file(acl_t ap) {
  acl_entry_t ae;
  int i;
  

  for (i = 0; acl_get_entry(ap, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &ae) == 1; i++) {
    acl_flagset_t fs;
    int fi;

    if (acl_get_flagset_np(ae, &fs) < 0)
      return -1;

    fi = acl_get_flag_np(fs, ACL_ENTRY_INHERITED);

    /* Remove all flags except for the INHERITED one */
    acl_clear_flags_np(fs);
    if (fi)
      acl_add_flag_np(fs, ACL_ENTRY_INHERITED);

    if (acl_set_flagset_np(ae, fs) < 0)
      return -1;
  }

  return 0;
}

	       

int
print_acl(FILE *fp,
	  acl_t a,
	  const char *path,
	  const struct stat *sp,
	  CONFIG *cfgp) {
  acl_entry_t ae;
  int i, is_trivial, d, len;
  uid_t *idp;
  char *as, *cp;
  char acebuf[2048], ubuf[64], gbuf[64], tbuf[80], *us, *gs;
  struct passwd *pp = NULL;
  struct group *gp = NULL;
  struct tm *tp;
  

  if (strncmp(path, "./", 2) == 0)
    path += 2;

  us = gs = NULL;
  pp = NULL;
  gp = NULL;
  if (sp) {
    pp = getpwuid(sp->st_uid);
    gp = getgrgid(sp->st_gid);
  }
  
  if (!pp) {
    snprintf(ubuf, sizeof(ubuf), "%u", sp->st_uid);
    us = s_dup(ubuf);
  } else
    us = s_dup(pp->pw_name);
  
  if (!gp) {
    snprintf(gbuf, sizeof(gbuf), "%u", sp->st_gid);
    gs = s_dup(gbuf);
  } else
    gs = s_dup(gp->gr_name);
  
  switch (cfgp->f_style) {
  case ACL_STYLE_DEFAULT:
    as = acl_to_text_np(a, NULL, (w_cfgp->f_verbose ? ACL_TEXT_VERBOSE|ACL_TEXT_APPEND_ID : 0));
    if (!as) {
      fprintf(stderr, "%s: Error: %s: Unable to display ACL\n", argv0, path);
      return 1;
    }

    if (w_c)
      putc('\n', fp);
    
    fprintf(fp, "# file: %s\n", path);
    if (cfgp->f_verbose)
      fprintf(fp, "# owner: %s (%d)\n", us, sp->st_uid);
    else
      fprintf(fp, "# owner: %s\n", us);
    if (cfgp->f_verbose)
      fprintf(fp, "# group: %s (%d)\n", gs, sp->st_gid);
    else
      fprintf(fp, "# group: %s\n", gs);
    fputs(as, fp);
    acl_free(as);
    break;
    
  case ACL_STYLE_CSV:
    /* One-liner, CSV-style */

    fprintf(fp, "%s;", path);
    for (i = 0; acl_get_entry(a, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &ae) == 1; i++) {
      if (i)
	putc(',', fp);
      ace2str(ae, acebuf, sizeof(acebuf));
      fprintf(fp, "%s", acebuf);
    }
    fprintf(fp, ";%d", sp->st_uid);
    fprintf(fp, ";%d", sp->st_gid);
    fprintf(fp, ";%s", us ? us : "-");
    fprintf(fp, ";%s", gs ? gs : "-");
    putc('\n', fp);
    break;

  case ACL_STYLE_BRIEF:
    /* One-liner */

    fprintf(fp, "%-22s", path);
    for (i = 0; acl_get_entry(a, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &ae) == 1; i++) {
      ace2str(ae, acebuf, sizeof(acebuf));
      cp = strchr(acebuf, ':');
      if (cp-acebuf == 1)
	cp = strchr(acebuf+2, ':');
      d = 10-(cp-acebuf);
      fprintf(fp, " %*s%s", d, "", acebuf);
    }
    putc('\n', fp);
    break;

  case ACL_STYLE_VERBOSE:
    if (w_c)
      putc('\n', fp);
    
    fprintf(fp, "# file: %s\n", path);
    for (i = 0; acl_get_entry(a, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &ae) == 1; i++) {
      char *cp;
      int len;
      acl_tag_t tt;

      acl_get_tag_type(ae, &tt);
      ace2str(ae, acebuf, sizeof(acebuf));

      cp = strchr(acebuf, ':');
      if (cp) {
	len = cp-acebuf;
	if (len > 0 && (tt == ACL_USER || tt == ACL_GROUP)) {
	  cp = strchr(cp+1, ':');
	  if (cp)
	    len = cp-acebuf;
	}
      } else
	len = 0;

      fprintf(fp, "%*s%s", (18-len), "", acebuf);
      switch (tt) {
      case ACL_USER_OBJ:
	if (us) {
	  if (cfgp->f_verbose)
	    fprintf(fp, "\t# %s (%d)", us, sp->st_uid);
	  else
	    fprintf(fp, "\t# %s", us);
	} else
	  fprintf(fp, "\t# (%d)", sp->st_uid);
	break;

      case ACL_GROUP_OBJ:
	if (gs) {
	  if (cfgp->f_verbose)
	    fprintf(fp, "\t# %s (%d)", gs, sp->st_gid);
	  else
	    fprintf(fp, "\t# %s", gs);
	} else
	  fprintf(fp, "\t# (%d)", sp->st_gid);
	break;

      case ACL_USER:
      case ACL_GROUP:
	if (cfgp->f_verbose) {
	  idp = (uid_t *) acl_get_qualifier(ae);
	  if (idp)
	    fprintf(fp, "\t# (%d)", *idp);
	}
	break;

      default:
	break;
      }
      putc('\n', fp);
    }
    break;
    
  case ACL_STYLE_SOLARIS:
    tp = localtime(&sp->st_mtime);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %R", tp);

    is_trivial = 0;
    acl_is_trivial_np(a, &is_trivial);
    
    if (w_c)
      putc('\n', fp);
    
    printf("%s%s %2lu %8s %8s %8ld %16s %s\n",
	   mode2str(sp->st_mode), is_trivial ? " " : "+",
	   (unsigned long int) sp->st_nlink,
	   us, gs,
	   sp->st_size,
	   tbuf, path);
	   
    as = acl_to_text_np(a, NULL, (w_cfgp->f_verbose ? ACL_TEXT_VERBOSE|ACL_TEXT_APPEND_ID : 0));
    if (!as) {
      fprintf(stderr, "%s: Error: %s: Unable to display ACL\n", argv0, path);
      return 1;
    }

    fputs(as, fp);
    acl_free(as);
    break;

  case ACL_STYLE_PRIMOS:
    if (w_c)
      putc('\n', fp);
    
    printf("ACL protecting \"%s\":\n", path);
    
    for (i = 0; acl_get_entry(a, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &ae) == 1; i++) {
      char *perms, *flags, *type;
      acl_tag_t tt;

      acl_get_tag_type(ae, &tt);
      ace2str(ae, acebuf, sizeof(acebuf));

      perms = strchr(acebuf, ':');
      *perms = '\0';
      if (tt == ACL_USER || tt == ACL_GROUP) {
	*perms++ = ':';
	perms = strchr(perms, ':');
      } else
	++perms;
      *perms++ = '\0';

      flags = strchr(perms, ':');
      *flags++ = '\0';
      
      type  = strchr(flags, ':');
      *type++ = '\0';

      fprintf(fp, "\t%15s:  %-13s  %-7s  %-5s", acebuf, perms, flags, type);
      switch (tt) {
      case ACL_USER_OBJ:
	if (us) {
	  if (cfgp->f_verbose)
	    fprintf(fp, "  # %s (%d)", us, sp->st_uid);
	  else 
	    fprintf(fp, "  # %s", us);
	} else
	  fprintf(fp, "  # (%d)", sp->st_uid);
	break;

      case ACL_GROUP_OBJ:
	if (gs) {
	  if (cfgp->f_verbose)
	    fprintf(fp, "  # %s (%d)", gs, sp->st_gid);
	  else
	    fprintf(fp, "  # %s", gs);
	} else
	  fprintf(fp, "  # (%d)", sp->st_gid);
	break;

      case ACL_USER:
      case ACL_GROUP:
	if (cfgp->f_verbose) {
	  idp = (uid_t *) acl_get_qualifier(ae);
	  if (idp)
	    fprintf(fp, "  # (%d)", *idp);
	}
	break;

      default:
	break;
      }

      putc('\n', fp);
    }
    break;
    
  case ACL_STYLE_SAMBA:
    if (w_c)
      putc('\n', fp);

    fprintf(fp, "FILENAME:%s\n", path);
    fprintf(fp, "REVISION:1\n");
    fprintf(fp, "CONTROL:SR|DP\n");

    if (pp)
      fprintf(fp, "OWNER:%s\n", us);
    else
      fprintf(fp, "OWNER:%d\n", sp->st_uid);

    if (gp)
      fprintf(fp, "GROUP:%s\n", gs);
    else
      fprintf(fp, "GROUP:%d\n", sp->st_gid);

    for (i = 0; acl_get_entry(a, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &ae) == 1; i++) {
      char *cp;
      ace2str_samba(ae, acebuf, sizeof(acebuf), sp);

      cp = strrchr(acebuf, '\t');
      if (*cp) {
	*cp++ = '\0';
	fprintf(fp, "%-50s\t# %s\n", acebuf, cp);
      } else
	fprintf(fp, "%s\n", acebuf);
    }
    break;
    
  case ACL_STYLE_ICACLS:
    if (w_c)
      putc('\n', fp);

    len = strlen(path);

    fprintf(fp, "%s", path);

    for (i = 0; acl_get_entry(a, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &ae) == 1; i++) {
      ace2str_icacls(ae, acebuf, sizeof(acebuf), sp);
      fprintf(fp, "%*s %s\n", i ? len : 0, "", acebuf);
    }
    break;
    
  default:
    return -1;
  }
  
  free(us);
  free(gs);
  return 0;
}


static acl_t 
get_acl(const char *path, 
	const struct stat *sp) {
  acl_t ap;
  struct stat sbuf;


  if (!sp) {
    if (lstat(path, &sbuf) < 0)
      return NULL;
    
    sp = &sbuf;
  }

  if (S_ISLNK(sp->st_mode))
    ap = acl_get_link_np(path, ACL_TYPE_NFS4);
  else
    ap = acl_get_file(path, ACL_TYPE_NFS4);
  if (!ap) {
    fprintf(stderr, "%s: Error: %s: Getting ACL: %s\n", argv0, path, strerror(errno));
    return NULL;
  }

  if (w_cfgp->f_sort) {
    acl_t sap = acl_sort(ap);
    if (!sap) {
      fprintf(stderr, "%s: Error: %s: Sorting ACL: %s\n", argv0, path, strerror(errno));
      return NULL;
    }
    acl_free(ap);
    ap = sap;
  }

  return ap;
}


static int
set_acl(const char *path,
	const struct stat *sp,
	acl_t ap,
	acl_t oap) {
  int rc;


  /* Skip set operation if old and new acl is the same */
  if (oap && acl_equal(ap, oap) == 1)
    return 0;

  rc = 0;
  if (!w_cfgp->f_noupdate) {
    if (S_ISLNK(sp->st_mode))
      rc = acl_set_link_np(path, ACL_TYPE_NFS4, ap);
    else
      rc = acl_set_file(path, ACL_TYPE_NFS4, ap);
  }

  if (rc < 0)
    return rc;

  return 1;
}

static int
walker_strip(const char *path,
	     const struct stat *sp,
	     size_t base,
	     size_t level,
	     void *vp) {
  int rc;
  acl_t ap, na;
  int tf;
  

  ap = get_acl(path, sp);
  if (!ap)
    return 1;

  tf = 0;
  if (acl_is_trivial_np(ap, &tf) < 0) {
    fprintf(stderr, "%s: Error: %s: Internal Error: %s\n", argv0, path, strerror(errno));
    acl_free(ap);
    return 1;
  }

  if (tf) {
    acl_free(ap);
    return 0;
  }
  
  na = acl_strip_np(ap, 0);

  rc = set_acl(path, sp, na, ap);

  acl_free(na);
  acl_free(ap);

  if (rc < 0) {
    fprintf(stderr, "%s: Error: %s: Setting ACL: %s\n", argv0, path, strerror(errno));
    return 1;
  }

  if (w_cfgp->f_verbose)
    printf("%s: ACL Stripped%s\n", path, (w_cfgp->f_noupdate ? " (NOT)" : ""));
  
  return 0;
}

static int
walker_delete(const char *path,
	      const struct stat *sp,
	      size_t base,
	      size_t level,
	      void *vp) {
  int rc;
  
  
  if (S_ISLNK(sp->st_mode))
    rc = acl_delete_link_np(path, ACL_TYPE_NFS4);
  else
    rc = acl_delete_file_np(path, ACL_TYPE_NFS4);
  
  if (rc < 0) {
    fprintf(stderr, "%s: Error: %s: Deleting ACL\n", argv0, path);
    return 1;
  }

  if (w_cfgp->f_verbose)
    printf("%s: ACL Deleted%s\n", path, (w_cfgp->f_noupdate ? " (NOT)" : ""));
  
  return 0;
}


static int
walker_sort(const char *path,
	    const struct stat *sp,
	    size_t base,
	    size_t level,
	    void *vp) {
  int rc;
  acl_t ap, na;


  ap = get_acl(path, sp);
  if (!ap) {
    fprintf(stderr, "%s: Error: %s: Getting ACL: %s\n", argv0, path, strerror(errno));
    return 1;
  }

  na = acl_sort(ap);
  if (!na) {
    fprintf(stderr, "%s: Error: %s: Sorting ACL: %s\n", argv0, path, strerror(errno));
    return 1;
  }

  rc = set_acl(path, sp, na, ap);

  acl_free(na);
  acl_free(ap);

  if (rc < 0) {
    fprintf(stderr, "%s: Error: %s: Setting ACL: %s\n", argv0, path, strerror(errno));
    return 1;
  }

  if (w_cfgp->f_verbose)
    printf("%s: ACL Sorted%s\n", path, (w_cfgp->f_noupdate ? " (NOT)" : ""));

  return 0;
}

#if 0
static int
walker_copy(const char *path,
	    const struct stat *sp,
	    size_t base,
	    size_t level,
	    void *vp) {
  int rc;
  acl_t ap = (acl_t) vp;

  
  if (!w_cfgp->f_noupdate) {
    if (S_ISLNK(sp->st_mode))
      rc = acl_set_link_np(path, ACL_TYPE_NFS4, ap);
    else
      rc = acl_set_file(path, ACL_TYPE_NFS4, ap);
    if (rc < 0) {
      fprintf(stderr, "%s: Error: %s: Setting ACL: %s\n", argv0, path, strerror(errno));
      return 1;
    }
  }

  if (w_cfgp->f_verbose)
    printf("%s: ACL Copied%s\n", path, (w_cfgp->f_noupdate ? " (NOT)" : ""));
  
  return 0;
}
#endif


typedef struct {
  acl_t da;
  acl_t fa;
} DACL;


static int
walker_set(const char *path,
	   const struct stat *sp,
	   size_t base,
	   size_t level,
	   void *vp) {
  int rc;
  DACL *a = (DACL *) vp;

  
  if (!w_cfgp->f_noupdate) {
    if (S_ISDIR(sp->st_mode))
      rc = set_acl(path, sp, a->da, NULL);
    else
      rc = set_acl(path, sp, a->fa, NULL);
    if (rc < 0) {
      fprintf(stderr, "%s: Error: %s: Setting ACL: %s\n", argv0, path, strerror(errno));
      return 1;
    }
  }
  
  if (w_cfgp->f_verbose)
    printf("%s: ACL Set%s\n", path, (w_cfgp->f_noupdate ? " (NOT)" : ""));
  
  return 0;
}

static int
 _acl_entry_pos(acl_t ap, 
		acl_tag_t tt, 
		acl_entry_type_t et,
		uid_t *ip) {
  int i;
  acl_entry_t ae;


  for (i = 0; acl_get_entry(ap, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &ae) == 1; i++) {
    acl_tag_t ott;
    acl_entry_type_t oet;
    uid_t *oip;


    acl_get_tag_type(ae, &ott);
    acl_get_entry_type_np(ae, &oet);
    oip = acl_get_qualifier(ae);

    if (ott > tt)
      return i;

    if (ott == tt) {
      if ((ott == ACL_USER || ott == ACL_GROUP)) {
	if (*oip < *ip)
	  continue;

	if (*oip == *ip) {
	  if (oet < et)
	    return i;
	}
      }
    }
  }

  return -1;
}


static int
walker_edit(const char *path,
	    const struct stat *sp,
	    size_t base,
	    size_t level,
	    void *vp) {
  int rc, i, j;
  acl_t oap, ap, nap;
  acl_entry_t nae;
  DACL *a = (DACL *) vp;


  oap = get_acl(path, sp);  
  if (!oap) {
    fprintf(stderr, "%s: Error: %s: Getting ACL: %s\n", argv0, path, strerror(errno));
    return 1;
  }

  ap = acl_dup(oap);
  if (!ap) {
    fprintf(stderr, "%s: Error: %s: Internal Fault (acl_dup): %s\n", argv0, path, strerror(errno));
    return 1;
  }

  if (S_ISDIR(sp->st_mode)) {
    nap = a->da;
  } else {
    nap = a->fa;
  }

  for (i = 0; acl_get_entry(nap, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &nae) == 1; i++) {
    acl_entry_t oae;
    acl_tag_t ntt;
    acl_entry_type_t net;
    uid_t *nip;
    int nm;
    acl_permset_t nps;
    acl_flagset_t nfs;


    acl_get_tag_type(nae, &ntt);
    acl_get_entry_type_np(nae, &net);
    nip = acl_get_qualifier(nae);
    
    if (acl_get_permset(nae, &nps) < 0 ||
	acl_get_flagset_np(nae, &nfs) < 0)
      goto Fail;
    
    nm = 0;

    for (j = 0; acl_get_entry(ap, j == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &oae) == 1; j++) {
      acl_tag_t ott;
      acl_entry_type_t oet;
      acl_permset_t ops;
      acl_flagset_t ofs;
      uid_t *oip;
      

      acl_get_tag_type(oae, &ott);
      acl_get_entry_type_np(oae, &oet);
      oip = acl_get_qualifier(oae);
      
      if (ott == ntt && oet == net) {
	if ((ott == ACL_USER || ott == ACL_GROUP) && (!oip || !nip || *oip != *nip))
	  continue;

	switch (nae->edit) {
	case '+':
	  if (acl_get_permset(oae, &ops) < 0 ||
	      acl_get_flagset_np(oae, &ofs) < 0)
	    goto Fail;

	  acl_merge_permset(ops, nps, +1);
	  acl_merge_flagset(ofs, nfs, +1);
	  if (acl_set_permset(oae, ops) < 0 ||
	      acl_set_flagset_np(oae, ofs) < 0)
	    goto Fail;
	  break;

	case '-':
	  if (acl_get_permset(oae, &ops) < 0 ||
	      acl_get_flagset_np(oae, &ofs) < 0)
	    goto Fail;

	  acl_merge_permset(ops, nps, -1);
	  acl_merge_flagset(ofs, nfs, -1);

	  if (acl_set_permset(oae, ops) < 0 ||
	      acl_set_flagset_np(oae, ofs) < 0)
	    goto Fail;
	  break;

	default:
	  if (acl_set_permset(oae, nps) < 0 ||
	      acl_set_flagset_np(oae, nfs) < 0)
	    goto Fail;
	}

	++nm;
      }
    }

    if (nm == 0 && !nae->edit) {
      int p;

      p = _acl_entry_pos(ap, ntt, net, nip);
      acl_create_entry_np(&ap, &oae, p);

      if (acl_set_tag_type(oae, ntt) < 0 ||
	  acl_set_permset(oae, nps) < 0 ||
	  acl_set_flagset_np(oae, nfs) < 0 ||
	  acl_set_entry_type_np(oae, net) < 0)
	goto Fail;

      if (ntt == ACL_USER || ntt == ACL_GROUP)
	acl_set_qualifier(oae, nip);
    }
  }

  gacl_clean(ap);

  rc = set_acl(path, sp, ap, oap);
  if (rc < 0) {
    fprintf(stderr, "%s: Error: %s: Setting ACL: %s\n", argv0, path, strerror(errno));
    goto Fail;
  }

  if (w_cfgp->f_verbose && rc > 0)
    printf("%s: ACL Updated%s\n", path, (w_cfgp->f_noupdate ? " (NOT)" : ""));
  if (w_cfgp->f_verbose > 1)
    print_acl(stdout, ap, path, sp, w_cfgp);
  
  return 0;

 Fail:
  acl_free(oap);
  acl_free(ap);
  return 1;
}


static int
walker_find(const char *path,
	    const struct stat *sp,
	    size_t base,
	    size_t level,
	    void *vp) {
  acl_t ap, map = (acl_t) vp;
  int i, j, nm;
  acl_entry_t ae, mae;


  ap = get_acl(path, sp);
  if (!ap) {
    /* Silently ignore this one - not ACL set? */
    return 0;
  }

  nm = 0;
  for (i = 0; acl_get_entry(ap, i == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &ae) == 1; i++) {
    acl_tag_t tt;
    acl_entry_type_t et;
    uid_t *idp;

    acl_get_tag_type(ae, &tt);
    acl_get_entry_type_np(ae, &et);
    idp = acl_get_qualifier(ae);

    for (j = 0; acl_get_entry(map, j == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &mae) == 1; j++) {
      acl_tag_t mtt;
      acl_entry_type_t met;
      uid_t *midp;
      
      acl_get_tag_type(mae, &mtt);
      acl_get_entry_type_np(mae, &met);
      midp = acl_get_qualifier(mae);

      if (tt == mtt && et == met) {
	if (tt == ACL_USER || tt == ACL_GROUP) {
	  if (idp && midp && *idp == *midp) {
	    ++nm;
	    break;
	  }
	} else {
	  ++nm;
	  break;
	}
      }
    }
  }

  if (nm > 0) {
    if (w_cfgp->f_verbose) {
      print_acl(stdout, ap, path, sp, w_cfgp);
      w_c++;
    }
    else
      puts(path);
  }

  return 0;
}

static int
walker_print(const char *path,
	     const struct stat *sp,
	     size_t base,
	     size_t level,
	     void *vp) {
  acl_t ap;
  FILE *fp;


  fp = stdout;

  ap = get_acl(path, sp);  
  if (!ap) {
    fprintf(stderr, "%s: Error: %s: Getting ACL: %s\n", argv0, path, strerror(errno));
    return 1;
  }
  
  print_acl(fp, ap, path, sp, w_cfgp);
  acl_free(ap);
  
  ++w_c;
  
  return 0;
}



int
_aclcmd_foreach(int argc,
		char **argv,
		CONFIG *cfgp,
		int (*handler)(const char *path,
			       const struct stat *sp,
			       size_t base,
			       size_t level,
			       void *vp),
		void *vp) {
  int i, rc = 0;
  

  w_cfgp = cfgp;
  w_c = 0;
  
  for (i = 0; rc == 0 && i < argc; i++) {
    rc = ft_foreach(argv[i], handler, vp,
		    cfgp->f_recurse ? -1 : cfgp->max_depth);
    if (rc) {
      if (rc < 0)
	fprintf(stderr, "%s: Error: %s: Accessing object: %s\n", 
		argv0, argv[i], strerror(errno));
      break;
    }
  }

  return rc;
}

int
aclcmd_list(int argc,
	    char **argv,
	    void *vp) {	       
  return _aclcmd_foreach(argc-1, argv+1, (CONFIG *) vp, walker_print, NULL);
}

int
aclcmd_get(int argc,
	   char **argv,
	   void *vp) {
  int i;
  size_t ns;
  char *nv;
  
  
  for (i = 1; i < argc; i++) {
    acl_t ap;
    char *pp, *as;

    pp = strchr(argv[i], '=');
    if (!pp) {
      fprintf(stderr, "%s: Error: %s: Missing required '=' character\n", argv0, argv[i]);
      return 1;
    }
    *pp++ = '\0';

    ap = get_acl(pp, NULL);
    if (!ap) {
      fprintf(stderr, "%s: Error: %s: Getting ACL: %s\n", argv0, pp, strerror(errno));
      return 1;
    }

    as = acl_to_text_np(ap, NULL, ACL_TEXT_COMPACT_NP);
    if (!as) {
      fprintf(stderr, "%s: Error: %s: Converting ACL to text: %s\n", argv0, pp, strerror(errno));
      acl_free(ap);
      return 1;
    }

    ns = strlen(argv[i])+1+strlen(as)+1;
    nv = malloc(ns);
    if (!nv) {
      fprintf(stderr, "%s: Error: Malloc(%d): %s\n", argv0, (int) ns, strerror(errno));
    }
    snprintf(nv, ns, "%s=%s", argv[i], as);
    putenv(nv);
    acl_free(as);
    acl_free(ap);
  }

  return 0;
}



int
aclcmd_copy(int argc,
	    char **argv,
	    void *vp) {	       
  int rc;
  struct stat s0;
  DACL a;

  
  if (lstat(argv[1], &s0) != 0) {
    fprintf(stderr, "%s: Error: %s: Accessing: %s\n", argv[0], argv[1], strerror(errno));
    return 1;
  }

  a.da = get_acl(argv[1], &s0);
  if (!a.da) {
    fprintf(stderr, "%s: Error: %s: Getting ACL: %s\n", argv[0], argv[1], strerror(errno));
    return 1;
  }

  a.fa = acl_dup(a.da);
  if (!a.fa) {
    acl_free(a.da);
    fprintf(stderr, "%s: Error: %s: Internal Fault (acl_dup): %s\n", argv[0], argv[1], strerror(errno));
    return 1;
  }
 
  _acl_filter_file(a.fa);

  rc = _aclcmd_foreach(argc-2, argv+2, (CONFIG *) vp, walker_set, (void *) &a);
  
  acl_free(a.da);
  acl_free(a.fa);
  return rc;
}

int
aclcmd_sort(int argc,
	    char **argv,
	    void *vp) {	       
  return _aclcmd_foreach(argc-1, argv+1, (CONFIG *) vp, walker_sort, NULL);
}


int
aclcmd_strip(int argc,
	     char **argv,
	     void *vp) {	       
  return _aclcmd_foreach(argc-1, argv+1, (CONFIG *) vp, walker_strip, NULL);
}

int
aclcmd_delete(int argc,
	      char **argv,
	      void *vp) {	       
  return _aclcmd_foreach(argc-1, argv+1, (CONFIG *) vp, walker_delete, NULL);
}


int
aclcmd_set(int argc,
	   char **argv,
	   void *vp) {
  int rc;
  DACL a;


  if (argc < 2) {
    fprintf(stderr, "%s: Error: Missing required arguments (<acl> <path>)\n", argv[0]);
    return 1;
  }


  a.da = acl_from_text(argv[1]);
  if (!a.da) {
    fprintf(stderr, "%s: Error: %s: Invalid ACL: %s\n", argv[0], argv[1], strerror(errno));
    return 1;
  }

  a.fa = acl_dup(a.da);
  if (!a.fa) {
    acl_free(a.da);
    fprintf(stderr, "%s: Error: %s: Invalid ACL: %s\n", argv[0], argv[1], strerror(errno));
    return 1;
  }

  rc = _aclcmd_foreach(argc-2, argv+2, (CONFIG *) vp, walker_set, (void *) &a);

  acl_free(a.da);
  acl_free(a.fa);

  return rc;
}

int
aclcmd_find(int argc,
	    char **argv,	
	    void *vp) {	       
  acl_t ap;


  if (argc < 2) {
    fprintf(stderr, "%s: Error: Missing required arguments (<acl> <path>)\n", argv[0]);
    return 1;
  }


  ap = acl_from_text(argv[1]);

  return _aclcmd_foreach(argc-2, argv+2, (CONFIG *) vp, walker_find, (void *) ap);
}

int
aclcmd_edit(int argc,
	    char **argv,
	    void *vp) {	       
  int rc;
  DACL a;


  if (argc < 2) {
    fprintf(stderr, "%s: Error: Missing required arguments (<acl> <path>)\n", argv[0]);
    return 1;
  }


  a.da = acl_from_text(argv[1]);
  if (!a.da) {
    fprintf(stderr, "%s: Error: %s: Invalid ACL: %s\n", argv[0], argv[1], strerror(errno));
    return 1;
  }

  a.fa = acl_dup(a.da);
  if (!a.fa) {
    acl_free(a.da);
    fprintf(stderr, "%s: Error: %s: Invalid dACL: %s\n", argv[0], argv[1], strerror(errno));
    return 1;
  }

  _acl_filter_file(a.fa);

  rc = _aclcmd_foreach(argc-2, argv+2, (CONFIG *) vp, walker_edit, (void *) &a);

  acl_free(a.da);
  acl_free(a.fa);

  return rc;
}

int
aclcmd_inherit(int argc,
	       char **argv,
	       void *vp) {	       
  fprintf(stderr, "%s: Error: %s: Not yet implemented\n", argv0, argv[0]);
  return 1;
}

int
aclcmd_check(int argc,
	     char **argv,
	     void *vp) {
  fprintf(stderr, "%s: Error: %s: Not yet implemented\n", argv0, argv[0]);
  return 1;
}


COMMAND acl_commands[] = {
  { "list-access", 	"<path>+",		aclcmd_list,	"List ACL(s)" },
  { "strip-access",     "<path>+",		aclcmd_strip,	"Strip ACL(s)" },
  { "sort-access",      "<path>+",		aclcmd_sort,	"Sort ACL(s)" },
  { "copy-access",      "<src> <dst>+",		aclcmd_copy,	"Copy ACL(s)" },
  { "delete-access",    "<path>+",		aclcmd_delete,	"Delete ACL(s)" },
  { "set-access",  	"<acl> <path>+",	aclcmd_set,	"Set ACL(s)" },
  { "edit-access",      "<path>+",		aclcmd_edit,	"Edit ACL(s)" },
  { "find-access",      "<path>+",		aclcmd_find,	"Search ACL(s)" },
  { "get-access", 	"<var>=<path>+",	aclcmd_get,	"Get ACL into variable" },
#if 0
  { "inherit-access",   "<path>+",		aclcmd_inherit,	"Propage ACL(s) inheritance" },
  { "check-access",     "<path>+",		aclcmd_check,	"Sanity-check ACL(s)" },
#endif
  { NULL,		NULL,			NULL,		NULL },
};
