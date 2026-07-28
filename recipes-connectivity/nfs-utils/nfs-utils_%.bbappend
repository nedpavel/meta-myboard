# This board uses nfs-utils only as an NFS *client* (the /mnt/dev dev-share
# automount). The kernel has no NFSD (server) support, so the auto-enabled
# nfs-server.service -- and its nfs-mountd / proc-fs-nfsd.mount dependencies --
# fail at boot ("Failed to mount NFSD configuration filesystem", etc.).
#
# Disable the server package's units via its preset; the separate
# nfs-utils-client package (nfs-client.target) stays enabled.
SYSTEMD_AUTO_ENABLE:${PN} = "disable"
