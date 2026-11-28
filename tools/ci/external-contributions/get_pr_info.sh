#!/bin/bash

# Script to fetch PR information and generate staging branch name
# Usage: ./get_pr_info.sh <PR_NUMBER> <GITHUB_TOKEN>
# Outputs: pr_number branch_name commit_sha staging_branch short_sha (space-separated)

set -euo pipefail

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    echo "Usage: $0 <PR_NUMBER> <GITHUB_TOKEN> <BASE_MASTER> [WORKFLOW_SCOPE]" >&2
    echo "Example: $0 123 \${{ github.token }} false diff-external" >&2
    exit 1
fi
PR_NUMBER="$1"
GITHUB_TOKEN="$2"
BASE_MASTER="$3"
WORKFLOW_SCOPE="${4:-}"

if ! [[ "$PR_NUMBER" =~ ^[0-9]+$ ]]; then
    echo "Error: PR_NUMBER must contain digits only (got '$PR_NUMBER')" >&2
    exit 1
fi

# Fetch PR details using GitHub API
if ! PR_INFO=$(curl -fsSL -H "Authorization: token $GITHUB_TOKEN" \
    "https://api.github.com/repos/memgraph/memgraph/pulls/$PR_NUMBER"); then
    echo "Error: Failed to fetch PR #$PR_NUMBER from GitHub API" >&2
    exit 1
fi

# Extract information using jq
BRANCH_NAME=$(echo "$PR_INFO" | jq -r '.head.ref // empty')
COMMIT_SHA=$(echo "$PR_INFO" | jq -r '.head.sha // empty')
REPO_URL=$(echo "$PR_INFO" | jq -r '.head.repo.clone_url // empty')
REPO_OWNER=$(echo "$PR_INFO" | jq -r '.head.user.login // empty')

# Validate that we got the required information
if [[ -z "$BRANCH_NAME" || ! "$COMMIT_SHA" =~ ^[0-9a-fA-F]{40}$ || -z "$REPO_URL" || -z "$REPO_OWNER" ]]; then
    echo "Error: Could not fetch valid PR information for PR #$PR_NUMBER" >&2
    echo "Response: $PR_INFO" >&2
    exit 1
fi

# Create staging branch name isolated per workflow/run scope when provided
STAGING_BRANCH="staging/${COMMIT_SHA:0:7}"
if [ -n "$WORKFLOW_SCOPE" ]; then
    SAFE_SCOPE="$(printf '%s' "$WORKFLOW_SCOPE" | tr -cs '[:alnum:]_-' '-' | sed 's/^-*//; s/-*$//')"
    if [ -n "$SAFE_SCOPE" ]; then
        STAGING_BRANCH="${STAGING_BRANCH}-${SAFE_SCOPE}"
    fi
fi
if [ "$BASE_MASTER" = "true" ]; then
    STAGING_BRANCH="${STAGING_BRANCH}-master"
fi
SHORT_SHA="${COMMIT_SHA:0:7}"

# Output all variables in a single line (space-separated)
echo "$PR_NUMBER $BRANCH_NAME $COMMIT_SHA $STAGING_BRANCH $SHORT_SHA $REPO_URL $REPO_OWNER"
