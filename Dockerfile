# Dockerfile for base collectd install
FROM ubuntu:18.04 as base

ENV DEBIAN_FRONTEND=noninteractive
ARG insight_version
ENV INSIGHT_VERSION=$insight_version

RUN apt-get update -y 
RUN apt-get upgrade -y
# Install all apt-get utils and required repos
RUN apt-get update && \
    apt-get upgrade -y && \
    # Install add-apt-repository
    apt-get install -y \
        software-properties-common && \
    apt-get update && \
    # Install
    apt-get install -y \
        # Install helper packages
        curl \
        unzip \
        git

# Install dependencies
RUN apt-get install -y \
        autoconf \
        automake \
        autotools-dev \
        bison \
        build-essential \
        curl \
        default-jdk \
        flex \
        g++ \
        git \
        iptables-dev \
        javahelper \
        libatasmart-dev \
        libbison-dev \
        libboost-all-dev \
        libboost-program-options-dev \
        libboost-test-dev \
        libcurl4-gnutls-dev \
        libdbi0-dev \
        libesmtp-dev \
        libevent-dev \
        libganglia1-dev \
        libgcrypt11-dev \
        libglib2.0-dev \
        libldap2-dev \
        libltdl-dev \
        liblvm2-dev \
        libmemcached-dev \
        libmnl-dev \
        libmysqlclient-dev \
        libnotify-dev \
        libopenipmi-dev \
        liboping-dev \
        libow-dev \
        libpcap0.8-dev \
        libperl-dev \
        libpq-dev \
        libprotobuf-c0-dev \
        librabbitmq-dev \
        librrd-dev \
        libsensors4-dev \
        libsnmp-dev>=5.4.2.1~dfsg-4~ \
        libssl-dev \
        libtool \
        libudev-dev \
        libupsclient-dev \
        libvarnishapi-dev \
        libvirt-dev>=0.4.0-6 \
        libxml2-dev \
        libyajl-dev \
        linux-libc-dev \
        pkg-config \
        protobuf-c-compiler \
        texinfo \
        wget

#pull thrift (dependency)
RUN git clone -b 0.10.0 https://github.com/apache/thrift.git

#build thrift
RUN cd /thrift && ./bootstrap.sh && ./configure --prefix=/usr --config-cache --disable-debug --with-java=no --with-erlang=no --with-php=no --with-perl=no --with-php_extension=no --with-ruby=no --with-haskell=no --with-go=no --with-libevent && make && make install

RUN cd /thrift/contrib/fb303 && ./bootstrap.sh && ./configure --prefix=/usr --disable-debug --with-thriftpath=/usr --without-java --without-php --without-python && make && make install

#build protobuf
RUN git clone -b v3.5.1 https://github.com/google/protobuf.git
RUN cd /protobuf && ./autogen.sh && ./configure --prefix=/usr --disable-debug && make && make install

#build protobuf-c
RUN git clone -b v1.3.0 https://github.com/protobuf-c/protobuf-c.git
RUN cd /protobuf-c && ./autogen.sh && ./configure --prefix=/usr --disable-debug && make && make install

RUN git clone -b v0.9.59 https://github.com/Karlson2k/libmicrohttpd
RUN cd libmicrohttpd && ./bootstrap && ./configure --prefix=/usr --disable-debug && make && make install

RUN apt-get install -y libc6-dbg

COPY . /collectd

RUN cd /collectd && ./clean.sh && ./build.sh && ./configure \
        --prefix /opt/collectd/usr \
        --with-data-max-name-len=1024 \
        --sysconfdir=/etc \
        --localstatedir=/var \
        --enable-debug       \
        --enable-all-plugins \
        --disable-ascent \
        --disable-rrdcached \
        --disable-lvm \
        --disable-write_kafka \
        --disable-curl_xml \
        --disable-dpdkstat \
        --disable-dpdkevents \
        --disable-grpc \
        --disable-gps \
        --disable-ipmi \
        --disable-lua \
        --disable-mqtt \
        --disable-modbus \
        --disable-intel_pmu \
        --disable-intel_rdt \
        --disable-static \
        --disable-write_riemann \
        --disable-zone \
        --disable-apple_sensors \
        --disable-lpar \
        --disable-tape \
        --disable-aquaero \
        --disable-mic \
        --disable-netapp \
        --disable-onewire \
        --disable-oracle \
        --disable-pf \
        --disable-python \
        --disable-redis \
        --disable-write_redis \
        --disable-routeros \
        --disable-rrdtool \
        # lm sensors lib will not work with version found in Ubuntu 20.04
        --disable-sensors \ 
        --disable-sigrok \
        --disable-write_mongodb \
        --disable-xmms \
        --disable-write_sensu \
        --disable-zfs-arc \
        --disable-tokyotyrant \
        --disable-write_kafka \
        --disable-barometer \
        --with-perl-bindings="INSTALLDIRS=vendor INSTALL_BASE=" \
        --without-libaquaero5 \
        --without-libstatgrab \
        --without-included-ltdl \
        --without-libgrpc++ \
        --without-libgps \
        --without-libmodbus \ 
        --without-libpython \
        --without-libsensors \
        --without-mic && make && make install 

# configure
RUN mkdir -p /opt/collectd/etc/collectd
RUN mkdir -p /opt/collectd/usr/bin
RUN mkdir -p /opt/collectd-symbols

ADD docker-build/templates/collectd/managed_config /opt/collectd/etc/collectd/
#COPY install-plugins.sh plugins.yaml /tmp/plugins/
#RUN bash /tmp/plugins/install-plugins.sh

ADD docker-build/collect-libs.sh docker-build/symbol-gen.sh /opt/
RUN /opt/symbol-gen.sh /opt/collectd /opt/collectd-symbols
RUN /opt/collect-libs.sh /opt/collectd /opt/collectd

# Clean up unnecessary man files
RUN rm -rf /opt/collectd/usr/share/man 

#CMD ["/bin/bash"]
FROM scratch as final-image

COPY --from=base /etc/ssl/certs/ca-certificates.crt /collectd/etc/ssl/certs/ca-certificates.crt
COPY --from=base /opt/collectd/ /collectd
COPY --from=base /opt/collectd-symbols/ /collectd-symbols
COPY docker-build/collectd_wrapper /collectd/usr/sbin

