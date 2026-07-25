#
# Formuła pakietu OpenWrt dla aqnapi (natywna wersja C).
# Buduje z LOKALNEGO checkoutu repo (bez pobierania z sieci ani sum kontrolnych) —
# feed zawiera całe repo, więc kompilujemy c/aqnapi.c bezpośrednio. Dzięki temu
# ta sama formuła działa i w gh-action-sdk (CI), i przy lokalnym feedzie src-link.
#
# Jedna definicja pakietu — SDK produkuje .ipk LUB .apk zależnie od wersji
# OpenWrt / CONFIG_USE_APK (patrz openwrt/README.md).
#
# Domyślnie build Z TLS (HTTPS: OpenSubtitles, napisy24 WWW/edit/delete, update,
# URL https://) — zależy od libmbedtls. Wariant bez TLS: odznacz w menuconfig
# "Enable HTTPS/TLS (mbedtls)".
#
# Copyright (C) 2026 aqnapi contributors — MIT
#

# Ścieżka do katalogu źródła (c/) rozwiązana względem tej formuły — MUSI być
# policzona PRZED include rules.mk (potem $(MAKEFILE_LIST) wskazuje inne pliki).
# $(realpath) rozwiązuje symlinki feeda i sekwencje "..".
AQNAPI_SRC:=$(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../../c)

include $(TOPDIR)/rules.mk

PKG_NAME:=aqnapi
PKG_VERSION:=1.0.16
PKG_RELEASE:=1

PKG_MAINTAINER:=aqnapi <adm@hycom.pl>
PKG_LICENSE:=MIT

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

# Źródło z lokalnego checkoutu — nadpisujemy domyślny unpack (nie ma PKG_SOURCE).
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(AQNAPI_SRC)/aqnapi.c $(PKG_BUILD_DIR)/aqnapi.c
endef

TLS_FLAGS:=
TLS_LIBS:=
ifeq ($(CONFIG_AQNAPI_TLS),y)
  TLS_FLAGS:=-DAQNAPI_TLS
  TLS_LIBS:=-lmbedtls -lmbedx509 -lmbedcrypto
endif

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -O2 -std=gnu11 $(TLS_FLAGS) \
		-o $(PKG_BUILD_DIR)/aqnapi $(PKG_BUILD_DIR)/aqnapi.c \
		$(TARGET_LDFLAGS) $(TLS_LIBS) -lz -lm
endef

define Package/aqnapi/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/aqnapi $(1)/usr/bin/aqnapi
endef

$(eval $(call BuildPackage,aqnapi))
