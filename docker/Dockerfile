FROM debian:stretch
LABEL maintainer="Fortuneblock Developers <dev@fortuneblock.xyz>"
LABEL description="Dockerised FortuneblockCore, built from Travis"

RUN apt-get update && apt-get -y upgrade && apt-get clean && rm -fr /var/cache/apt/*

COPY bin/* /usr/bin/
