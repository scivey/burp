#include "include.h"
#include "../../cmd.h"
#include "../../hexmap.h"
#include "../../server/protocol2/restore.h"
#include "../../sbuf.h"
#include "../../slist.h"
#include "dpth.h"

#include <librsync.h>

static int create_zero_length_file(const char *path)
{
	int ret=0;
	struct fzp *dest;
	if(!(dest=fzp_open(path, "wb")))
		ret=-1;
	ret|=fzp_close(&dest);
	return ret;
}

static int inflate_or_link_oldfile(struct asfd *asfd, const char *oldpath,
	const char *infpath, struct conf **cconfs, int compression)
{
	int ret=0;
	struct stat statp;

	if(lstat(oldpath, &statp))
	{
		logp("could not lstat %s\n", oldpath);
		return -1;
	}

	if(dpth_protocol1_is_compressed(compression, oldpath))
	{
		//logp("inflating...\n");

		if(!statp.st_size)
		{
			// Empty file - cannot inflate.
			logp("asked to inflate zero length file: %s\n",
				oldpath);
			return create_zero_length_file(infpath);
		}

		if((ret=zlib_inflate(asfd, oldpath, infpath, cconfs)))
			logp("zlib_inflate returned: %d\n", ret);
	}
	else
	{
		// Not compressed - just hard link it.
		if(do_link(oldpath, infpath, &statp, cconfs,
			1 /* allow overwrite of infpath */))
				return -1;
	}
	return ret;
}

static int send_file(struct asfd *asfd, struct sbuf *sb,
	int patches, const char *best,
	unsigned long long *bytes, struct conf **cconfs)
{
	int ret=0;
	static BFILE *bfd=NULL;

	if(!bfd && !(bfd=bfile_alloc())) return -1;

	bfile_init(bfd, 0, cconfs);
	if(bfd->open_for_send(bfd, asfd, best, sb->winattr,
		1 /* no O_NOATIME */, cconfs)) return -1;
	//logp("sending: %s\n", best);
	if(asfd->write(asfd, &sb->path))
		ret=-1;
	else if(patches)
	{
		// If we did some patches, the resulting file
		// is not gzipped. Gzip it during the send. 
		ret=send_whole_file_gzl(asfd, best, sb->protocol1->datapth.buf,
			1, bytes, NULL, cconfs, 9, bfd, NULL, 0);
	}
	else
	{
		// If it was encrypted, it may or may not have been compressed
		// before encryption. Send it as it as, and let the client
		// sort it out.
		if(sb->path.cmd==CMD_ENC_FILE
		  || sb->path.cmd==CMD_ENC_METADATA
		  || sb->path.cmd==CMD_ENC_VSS
		  || sb->path.cmd==CMD_ENC_VSS_T
		  || sb->path.cmd==CMD_EFS_FILE)
		{
			ret=send_whole_filel(asfd, sb->path.cmd, best,
				sb->protocol1->datapth.buf, 1, bytes,
				cconfs, bfd, NULL, 0);
		}
		// It might have been stored uncompressed. Gzip it during
		// the send. If the client knew what kind of file it would be
		// receiving, this step could disappear.
		else if(!dpth_protocol1_is_compressed(sb->compression,
			sb->protocol1->datapth.buf))
		{
			ret=send_whole_file_gzl(asfd,
				best, sb->protocol1->datapth.buf, 1, bytes,
				NULL, cconfs, 9, bfd, NULL, 0);
		}
		else
		{
			// If we did not do some patches, the resulting
			// file might already be gzipped. Send it as it is.
			ret=send_whole_filel(asfd, sb->path.cmd, best,
				sb->protocol1->datapth.buf, 1, bytes,
				cconfs, bfd, NULL, 0);
		}
	}
	bfd->close(bfd, asfd);
	return ret;
}

static int verify_file(struct asfd *asfd, struct sbuf *sb,
	int patches, const char *best,
	unsigned long long *bytes, struct conf **cconfs)
{
	MD5_CTX md5;
	size_t b=0;
	const char *cp=NULL;
	const char *newsum=NULL;
	uint8_t in[ZCHUNK];
	uint8_t checksum[MD5_DIGEST_LENGTH];
	unsigned long long cbytes=0;
	struct fzp *fzp=NULL;

	if(!(cp=strrchr(sb->protocol1->endfile.buf, ':')))
	{
		logw(asfd, cconfs,
			"%s has no md5sum!\n", sb->protocol1->datapth.buf);
		return 0;
	}
	cp++;
	if(!MD5_Init(&md5))
	{
		logp("MD5_Init() failed\n");
		return -1;
	}
	if(patches
	  || sb->path.cmd==CMD_ENC_FILE
	  || sb->path.cmd==CMD_ENC_METADATA
	  || sb->path.cmd==CMD_EFS_FILE
	  || sb->path.cmd==CMD_ENC_VSS
	  || (!patches && !dpth_protocol1_is_compressed(sb->compression, best)))
		fzp=fzp_open(best, "rb");
	else
		fzp=fzp_gzopen(best, "rb");

	if(!fzp)
	{
		logw(asfd, cconfs, "could not open %s\n", best);
		return 0;
	}
	while((b=fzp_read(fzp, in, ZCHUNK))>0)
	{
		cbytes+=b;
		if(!MD5_Update(&md5, in, b))
		{
			logp("MD5_Update() failed\n");
			fzp_close(&fzp);
			return -1;
		}
	}
	if(!fzp_eof(fzp))
	{
		logw(asfd, cconfs, "error while reading %s\n", best);
		fzp_close(&fzp);
		return 0;
	}
	fzp_close(&fzp);
	if(!MD5_Final(checksum, &md5))
	{
		logp("MD5_Final() failed\n");
		return -1;
	}
	newsum=bytes_to_md5str(checksum);

	if(strcmp(newsum, cp))
	{
		logp("%s %s\n", newsum, cp);
		logw(asfd, cconfs, "md5sum for '%s (%s)' did not match!\n",
			sb->path.buf, sb->protocol1->datapth.buf);
		logp("md5sum for '%s (%s)' did not match!\n",
			sb->path.buf, sb->protocol1->datapth.buf);
		return 0;
	}
	*bytes+=cbytes;

	// Just send the file name to the client, so that it can show cntr.
	if(asfd->write(asfd, &sb->path)) return -1;
	return 0;
}

static int process_data_dir_file(struct asfd *asfd,
	struct bu *bu, struct bu *b, const char *path,
	struct sbuf *sb, enum action act, struct sdirs *sdirs,
	struct conf **cconfs)
{
	int ret=-1;
	int patches=0;
	char *dpath=NULL;
	struct stat dstatp;
	const char *tmp=NULL;
	const char *best=NULL;
	unsigned long long bytes=0;
	static char *tmppath1=NULL;
	static char *tmppath2=NULL;

	if((!tmppath1 && !(tmppath1=prepend_s(sdirs->client, "tmp1")))
	  || (!tmppath2 && !(tmppath2=prepend_s(sdirs->client, "tmp2"))))
		goto end;

	best=path;
	tmp=tmppath1;
	// Now go down the list, applying any deltas.
	for(b=b->prev; b && b->next!=bu; b=b->prev)
	{
		free_w(&dpath);
		if(!(dpath=prepend_s(b->delta, sb->protocol1->datapth.buf)))
			goto end;

		if(lstat(dpath, &dstatp) || !S_ISREG(dstatp.st_mode))
			continue;

		if(!patches)
		{
			// Need to gunzip the first one.
			if(inflate_or_link_oldfile(asfd, best, tmp,
				cconfs, sb->compression))
			{
				char msg[256]="";
				snprintf(msg, sizeof(msg),
				  "error when inflating %s\n", best);
				log_and_send(asfd, msg);
				goto end;
			}
			best=tmp;
			if(tmp==tmppath1) tmp=tmppath2;
			else tmp=tmppath1;
		}

		if(do_patch(asfd, best, dpath, tmp,
			0 /* do not gzip the result */,
			sb->compression /* from the manifest */, cconfs))
		{
			char msg[256]="";
			snprintf(msg, sizeof(msg), "error when patching %s\n",
				path);
			log_and_send(asfd, msg);
			goto end;
		}

		best=tmp;
		if(tmp==tmppath1) tmp=tmppath2;
		else tmp=tmppath1;
		unlink(tmp);
		patches++;
	}

	switch(act)
	{
		case ACTION_RESTORE:
			if(send_file(asfd, sb, patches, best, &bytes, cconfs))
				goto end;
			break;
		case ACTION_VERIFY:
			if(verify_file(asfd, sb, patches, best, &bytes, cconfs))
				goto end;
			break;
		default:
			logp("Unknown action: %d\n", act);
			goto end;
	}
	cntr_add(get_cntr(cconfs[OPT_CNTR]), sb->path.cmd, 0);
	cntr_add_bytes(get_cntr(cconfs[OPT_CNTR]),
		  strtoull(sb->protocol1->endfile.buf, NULL, 10));
	cntr_add_sentbytes(get_cntr(cconfs[OPT_CNTR]), bytes);

	ret=0;
end:
	free_w(&dpath);
	return ret;
}

// a = length of struct bu array
// i = position to restore from
static int restore_file(struct asfd *asfd, struct bu *bu,
	struct sbuf *sb, enum action act,
	struct sdirs *sdirs, struct conf **cconfs)
{
	int ret=-1;
	char *path=NULL;
	struct bu *b;
	struct bu *hlwarn=NULL;
	struct stat statp;

	// Go up the array until we find the file in the data directory.
	for(b=bu; b; b=b->next)
	{
		free_w(&path);
		if(!(path=prepend_s(b->data, sb->protocol1->datapth.buf)))
			goto end;

		if(lstat(path, &statp) || !S_ISREG(statp.st_mode))
			continue;

		if(b!=bu && (bu->flags & BU_HARDLINKED)) hlwarn=b;

		if(process_data_dir_file(asfd, bu, b,
			path, sb, act, sdirs, cconfs)) goto end;

		// This warning must be done after everything else,
		// Because the client does not expect another cmd after
		// the warning.
		if(hlwarn) logw(asfd, cconfs, "restore found %s in %s\n",
			sb->path.buf, hlwarn->basename);
		ret=0; // All OK.
		break;
	}

	if(!b) logw(asfd, cconfs, "restore could not find %s (%s)\n",
		sb->path.buf, sb->protocol1->datapth.buf);
end:
	free_w(&path);
	return ret;
}

int restore_sbuf_protocol1(struct asfd *asfd, struct sbuf *sb, struct bu *bu,
	enum action act, struct sdirs *sdirs,
	enum cntr_status cntr_status, struct conf **cconfs)
{
	if((sb->protocol1->datapth.buf
		&& asfd->write(asfd, &(sb->protocol1->datapth)))
	  || asfd->write(asfd, &sb->attr))
		return -1;
	else if(sbuf_is_filedata(sb))
	{
		if(!sb->protocol1->datapth.buf)
		{
			logw(asfd, cconfs,
				"Got filedata entry with no datapth: %c:%s\n",
					sb->path.cmd, sb->path.buf);
			return 0;
		}
		return restore_file(asfd, bu, sb, act, sdirs, cconfs);
	}
	else
	{
		if(asfd->write(asfd, &sb->path))
			return -1;
		// If it is a link, send what
		// it points to.
		else if(sbuf_is_link(sb)
		  && asfd->write(asfd, &sb->link)) return -1;
		cntr_add(get_cntr(cconfs[OPT_CNTR]), sb->path.cmd, 0);
	}
	return 0;
}