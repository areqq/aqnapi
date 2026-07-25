#
# Formuła pakietu OpenWrt dla aqnapi (natywna wersja C).
# Jedna definicja pakietu — SDK produkuje .ipk LUB .apk zależnie od wersji
# OpenWrt / CONFIG_USE_APK (patrz openwrt/README.md).
#
# Domyślnie build Z TLS (HTTPS: OpenSubtitles, napisy24 WWW/edit/delete, update,
# URL https://) — zależy od libmbedtls. Wariant bez TLS: odznacz w menuconfig
# "Enable HTTPS/TLS (mbedtls)" (Config/aqnapi niżej).
#
# Copyright (C) 2026 aqnapi contributors — MIT
#

include $(TOPDIR)/rules.mk

PKG_NAME:=aqnapi
PKG_VERSION:=1.0.15
PKG_RELEASE:=1

# Źródło: klon taga vX.Y.Z z GitHuba (c/aqnapi.c ląduje w $(PKG_BUILD_DIR)/c/).
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/areqq/aqnapi.git
PKG_SOURCE_VERSION:=v$(PKG_VERSION)
PKG_MIRROR_HASH:=skip
# ^ Zamień "skip" na realny sha256, aby włączyć weryfikację (odtwarzalny build):
#     make package/aqnapi/download V=s
#     sha256sum dl/aqnapi-*.tar.*

PKG_MAINTAINER:=aqnapi <adm@hycom.pl>
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk

define Package/aqnapi/config
	config AQNAPI_TLS
		bool "Enable HTTPS/TLS (mbedtls)"
		depends on PACKAGE_aqnapi
		default y
		help
		  Włącz obsługę HTTPS (OpenSubtitles, napisy24 WWW/edit/delete,
		  self-update, adresy https://). Dodaje zależność od libmbedtls.
		  Bez tego dostępne są tylko operacje po HTTP.
endef

define Package/aqnapi
  SECTION:=multimedia
  CATEGORY:=Multimedia
  TITLE:=Subtitle client (Napisy24 / napiprojekt / OpenSubtitles)
  URL:=https://github.com/areqq/aqnapi
  DEPENDS:=+zlib +AQNAPI_TLS:libmbedtls +AQNAPI_TLS:ca-bundle
endef

define Package/aqnapi/description
  Jednoplikowy natywny klient CLI do trzech serwisów napisów: Napisy24.pl,
  napiprojekt.pl i OpenSubtitles.com. Pobieranie/wysyłanie napisów po haszu
  pliku, wyszukiwanie, konwersja formatów do SRT (UTF-8+BOM, LF),
  synchronizacja, merge/split. Bez zależności od interpretera — czysty C.

  Bez TLS: operacje po HTTP (download/get/search, całe napiprojekt,
  większość napisy24). Z TLS (libmbedtls): dodatkowo OpenSubtitles,
  napisy24 WWW/edit/delete, self-update i adresy https://.
endef

TLS_FLAGS:=
TLS_LIBS:=
ifeq ($(CONFIG_AQNAPI_TLS),y)
  TLS_FLAGS:=-DAQNAPI_TLS
  TLS_LIBS:=-lmbedtls -lmbedx509 -lmbedcrypto
endif

# Pojedynczy plik C — kompilujemy bezpośrednio toolchainem OpenWrt.
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -O2 -std=gnu11 $(TLS_FLAGS) \
		-o $(PKG_BUILD_DIR)/aqnapi $(PKG_BUILD_DIR)/c/aqnapi.c \
		$(TARGET_LDFLAGS) $(TLS_LIBS) -lz -lm
endef

define Package/aqnapi/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/aqnapi $(1)/usr/bin/aqnapi
endef

$(eval $(call BuildPackage,aqnapi))
