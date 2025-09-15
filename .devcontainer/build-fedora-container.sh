#!/bin/bash -x
podman buildx build --load --build-arg BUILDKIT_INLINE_CACHE=1 \
  -f /tmp/devcontainercli-tpasch/container-features/0.77.0-1750664255467/Dockerfile-with-features \
  -t vsc-snapcast-e574d9a73cd47a3915ad1e04357bed75a419087adf9a2ab258da25610fc9313e \
  --target dev_containers_target_stage \
  --build-arg VARIANT=42 \
  --build-arg _DEV_CONTAINERS_BASE_IMAGE=dev_container_auto_added_stage_label \
  .
