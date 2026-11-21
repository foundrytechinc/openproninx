#!/bin/bash

# Arg 1
SRC_DIR=$1
if [ "${SRC_DIR}" = "" ]; then
	SRC_DIR=${HOME}/__makefs/src
	mkdir -p "$(dirname "${SRC_DIR}")" || exit 1
fi

if [ ! -d "${SRC_DIR}" ]; then
	echo "No such directory ${SRC_DIR}"
	exit 1
fi

if [ "${MTREE}" = "" ]; then
	MTREE=./src/mtree
fi
if [ ! -x "${MTREE}" ]; then
	echo "No such exeutable ${MTREE}"
	exit 1
fi

if ! which mtree >/dev/null 2>&1; then
	echo "No mtree"
	exit 0
fi

echo "mtree -c -p ${SRC_DIR} | ${MTREE} -p ${SRC_DIR}"
mtree -c -p "${SRC_DIR}" | "${MTREE}" -p "${SRC_DIR}" || exit 1

echo "${MTREE} -c -p ${SRC_DIR} | mtree -p ${SRC_DIR}"
"${MTREE}" -c -p "${SRC_DIR}" | mtree -p "${SRC_DIR}" || exit 1

echo "success"
