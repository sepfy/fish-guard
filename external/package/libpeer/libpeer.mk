################################################################################
#
# libpeer 
#
################################################################################

LIBPEER_SITE = ../libpeer
LIBPEER_SITE_METHOD = local
LIBPEER_INSTALL_STAGING = YES
$(eval $(cmake-package))
