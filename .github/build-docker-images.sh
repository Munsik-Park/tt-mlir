#!/bin/bash
# SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

set -e

REPO=tenstorrent/tt-mlir
BASE_IMAGE_NAME=ghcr.io/$REPO/tt-mlir-base-ubuntu-22-04
CI_IMAGE_NAME=ghcr.io/$REPO/tt-mlir-ci-ubuntu-22-04
BASE_IRD_IMAGE_NAME=ghcr.io/$REPO/tt-mlir-base-ird-ubuntu-22-04
IRD_IMAGE_NAME=ghcr.io/$REPO/tt-mlir-ird-ubuntu-22-04
CIBW_IMAGE_NAME=ghcr.io/$REPO/tt-mlir-manylinux-2-34

# Compute the hash of the Dockerfile
DOCKER_TAG=$(./.github/get-docker-tag.sh)
echo "Docker tag: $DOCKER_TAG"

# Are we on main branch
ON_MAIN=$(git branch --show-current | grep -q main && echo "true" || echo "false")

build_and_push() {
    local image_name=$1
    local dockerfile=$2
    local on_main=$3
    local from_image=$4


    if docker manifest inspect $image_name:$DOCKER_TAG > /dev/null; then
        echo "Image $image_name:$DOCKER_TAG already exists"
    else
        echo "Building image $image_name:$DOCKER_TAG"
        docker build \
            --progress=plain \
            --build-arg FROM_TAG=$DOCKER_TAG \
            ${from_image:+--build-arg FROM_IMAGE=$from_image} \
            -t $image_name:$DOCKER_TAG \
            -t $image_name:latest \
            -f $dockerfile .

        echo "Pushing image $image_name:$DOCKER_TAG"
        docker push $image_name:$DOCKER_TAG
    fi

    # If we are on main branch also push the latest tag
    if [ "$on_main" = "true" ]; then
        printf "\nPushing latest tag for $image_name"
        # Used to push both tags in one command on the original sha256 sum layer remotely (docker create manifest create a new sha256 sum only with only with new tag)
        docker buildx imagetools create $image_name:$DOCKER_TAG --tag $image_name:latest --tag $image_name:$DOCKER_TAG
    fi
}

build_and_push $BASE_IMAGE_NAME .github/Dockerfile.base $ON_MAIN
build_and_push $BASE_IRD_IMAGE_NAME .github/Dockerfile.ird $ON_MAIN base
build_and_push $CI_IMAGE_NAME .github/Dockerfile.ci $ON_MAIN
build_and_push $IRD_IMAGE_NAME .github/Dockerfile.ird $ON_MAIN ci
build_and_push $CIBW_IMAGE_NAME .github/Dockerfile.cibuildwheel $ON_MAIN

echo "All images built and pushed successfully"
echo "CI_IMAGE_NAME:"
echo $CI_IMAGE_NAME:$DOCKER_TAG
