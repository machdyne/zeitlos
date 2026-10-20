#ifndef DRAW_HOSTFS_H
#define DRAW_HOSTFS_H

typedef struct {
	int	open_write;		// fs_open_write() fails
	int	write_at;		// the Nth fs_write_chunk() fails (1-based; 0 never)
	int	write_short;	// ... by writing half, rather than returning -1
	int	read_at;		// the Nth fs_read_chunk() fails
	int	close;			// fs_close_handle() reports failure (after closing)
	int	unlink;			// fs_unlink() fails
	int	rename;			// fs_rename() fails (when the target is free)
} hostfs_faults_t;

extern hostfs_faults_t hostfs_fault;

void hostfs_reset(void);
int hostfs_open_count(void);

#endif
