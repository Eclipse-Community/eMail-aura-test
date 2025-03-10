# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

ifndef CONFIG_DIR
$(error CONFIG_DIR must be set before including makensis.mk)
endif

ABS_CONFIG_DIR := $(abspath $(CONFIG_DIR))

SFX_MODULE ?= $(error SFX_MODULE is not defined)

TOOLKIT_NSIS_FILES = \
	common.nsh \
	locale.nlf \
	locale-fonts.nsh \
	locale-rtl.nlf \
	locales.nsi \
	overrides.nsh \
	setup.ico \
	$(NULL)

CUSTOM_NSIS_PLUGINS = \
	AccessControl.dll \
	AppAssocReg.dll \
	ApplicationID.dll \
	CertCheck.dll \
	CityHash.dll \
	InetBgDL.dll \
	InvokeShellVerb.dll \
	liteFirewallW.dll \
	ServicesHelper.dll \
	ShellLink.dll \
	UAC.dll \
	$(NULL)

CUSTOM_UI = \
	nsisui.exe \
	$(NULL)

$(CONFIG_DIR)/setup.exe::
	$(INSTALL) $(addprefix $(MOZILLA_DIR)/aura/packager/nsis/,$(TOOLKIT_NSIS_FILES)) $(CONFIG_DIR)
	$(INSTALL) $(addprefix $(MOZILLA_DIR)/other-licenses/nsis/Plugins/,$(CUSTOM_NSIS_PLUGINS)) $(CONFIG_DIR)
	$(INSTALL) $(addprefix $(MOZILLA_DIR)/other-licenses/nsis/,$(CUSTOM_UI)) $(CONFIG_DIR)
	cd $(CONFIG_DIR) && $(MAKENSISU) installer.nsi

$(CONFIG_DIR)/7zSD.sfx:
	$(CYGWIN_WRAPPER) upx --best -o $(CONFIG_DIR)/7zSD.sfx $(SFX_MODULE)

installer::
	$(INSTALL) $(CONFIG_DIR)/setup.exe $(DEPTH)/installer-stage
	cd $(DEPTH)/installer-stage && $(CYGWIN_WRAPPER) 7z a -r -t7z $(ABS_CONFIG_DIR)/app.7z -mx -m0=BCJ2 -m1=LZMA:d25 -m2=LZMA:d19 -m3=LZMA:d19 -mb0:1 -mb0s1:2 -mb0s2:3
	$(MAKE) $(CONFIG_DIR)/7zSD.sfx
	$(NSINSTALL) -D $(DIST)/$(PKG_INST_PATH)
	cat $(CONFIG_DIR)/7zSD.sfx $(CONFIG_DIR)/app.tag $(CONFIG_DIR)/app.7z > "$(DIST)/$(PKG_INST_PATH)$(PKG_INST_BASENAME).exe"
	chmod 0755 "$(DIST)/$(PKG_INST_PATH)$(PKG_INST_BASENAME).exe"

# For building the uninstaller during the application build so it can be
# included for mar file generation.
uninstaller::
	$(INSTALL) $(addprefix $(MOZILLA_DIR)/aura/packager/nsis/,$(TOOLKIT_NSIS_FILES)) $(CONFIG_DIR)
	$(INSTALL) $(addprefix $(MOZILLA_DIR)/other-licenses/nsis/Plugins/,$(CUSTOM_NSIS_PLUGINS)) $(CONFIG_DIR)
	cd $(CONFIG_DIR) && $(MAKENSISU) uninstaller.nsi
	$(NSINSTALL) -D $(DIST)/bin/uninstall
	cp $(CONFIG_DIR)/helper.exe $(DIST)/bin/uninstall
