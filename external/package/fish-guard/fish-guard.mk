################################################################################
#
# fish guard 
#
################################################################################

FISH_GUARD_SITE = ../external/package/fish-guard/src
FISH_GUARD_SITE_METHOD = local
FISH_GUARD_DEPENDENCIES = libpeer
$(eval $(cmake-package))
