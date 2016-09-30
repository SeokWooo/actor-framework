#!/bin/bash

# exit on first error
set -e

usage_string="\
Usage: $0 VERSION
"

function usage() {
  echo "$usage_string"
  exit
}

function raise_error() {
  echo "*** $1" 1>&2
  exit
}

function file_ending() {
  echo $(basename "$1" | awk -F '.' '{if (NF > 1) print $NF}')
}

function assert_exists() {
  while [ $# -gt 0 ]; do
    if [ -z "$(file_ending "$1")" ]; then
      # assume directory for paths without file ending
      if [ ! -d "$1" ] ; then
        raise_error "directory $1 missing"
      fi
    else
      if [ ! -f "$1" ]; then
        raise_error "file $1 missing"
      fi
    fi
    shift
  done
}

function assert_exists_not() {
  while [ $# -gt 0 ]; do
    if [ -e "$1" ]; then
        raise_error "file $1 already exists"
    fi
    shift
  done
}

function assert_git_status_clean() {
  # save current directory
  anchor="$PWD"
  cd "$1"
  # check for untracked files
  untracked_files=$(git ls-files --others --exclude-standard)
  if [ -n "$untracked_files" ]; then
    raise_error "$1 contains untracked files"
  fi
  # check Git status
  if [ -n "$(git status --porcelain)" ] ; then
    raise_error "$1 is not in a clean state (see git status)"
  fi
  # restore directory
  cd "$anchor"
}

function ask_permission() {
  yes_or_no=""
  while [ "$yes_or_no" != "n" ] && [ "$yes_or_no" != "y" ]; do
    echo ">>> $1"
    read yes_or_no
  done
  if [ "$yes_or_no" = "n" ]; then
    raise_error "aborted"
  fi
}

if [ $# -ne 1  ] ; then
  usage
fi

echo "\

                         ____    _    _____
                        / ___|  / \  |  ___|    C++
                       | |     / _ \ | |_       Actor
                       | |___ / ___ \|  _|      Framework
                        \____/_/   \_|_|

This script expects to run at the root directory of a Git clone of CAF.
The current repository must be develop, there must be no untracked file,
and the working tree status must be equal to the current HEAD commit.
Further, the script expects a relase_note.md file in the current directory
with the developer blog checked out one level above, i.e.:

\$HOME
├── .github-oauth-token

.
├── libcaf_io
├── blog_release_note.md
├── github_release_note.md

..
├── blog
│   ├── _posts

"

# assumed files
token_path="$HOME/.github-oauth-token"
blog_msg="blog_release_note.md"
github_msg="github_release_note.md"
config_hpp_path="libcaf_core/caf/config.hpp"

# assumed directories
blog_posts_path="../blog/_posts"

# check whether all expected files and directories exist
assert_exists "$token_path" "$config_hpp_path" "$blog_msg" "$github_msg" "$blog_posts_path"

# check whether curl command is safe to execute
# (GitHub script must not have any ' because it breaks the temporary script)
if [ -n "$(grep "'" "$github_msg")" ] ; then
  raise_error "$github_msg must not contain any '"
fi

# target files
blog_target_file="$blog_posts_path/$(date +%F)-version-$1-released.md"
assert_exists_not "$blog_target_file" .make-release-steps.bash

assert_git_status_clean "."
assert_git_status_clean "../blog/"

# convert major.minor.patch version given as first argument into JJIIPP with:
#   JJ: two-digit (zero padded) major version
#   II: two-digit (zero padded) minor version
#   PP: two-digit (zero padded) patch version
# but omit leading zeros
version_str=$(echo "$1" | awk -F. '{ if ($1 > 0) printf("%d%02d%02d\n", $1, $2, $3); else printf("%02d%02d\n", $2, $3)  }')

echo ">>> patching config.hpp"
sed "s/#define CAF_VERSION [0-9]*/#define CAF_VERSION ${version_str}/g" < "$config_hpp_path" > .tmp_conf_hpp
mv .tmp_conf_hpp "$config_hpp_path"

echo ; echo
echo ">>> please review the diff reported by Git for patching config.hpp:"
git diff
echo ; echo
ask_permission "type [n] to abort or [y] for commiting and pushing it"

# piping through AWK/printf makes sure 0.15 is expanded to 0.15.0
tag_version=$(echo "$1" | awk -F. '{ printf("%d.%d.%d", $1, $2, $3)  }')

token=$(cat "$token_path")
tag_descr=$(awk 1 ORS='\\r\\n' "$github_msg")
github_json=$(printf '{"tag_name": "%s","name": "%s","body": "%s","draft": false,"prerelease": false}' "$tag_version" "$tag_version" "$tag_descr")

# for returning to this directory after pushing blog
anchor="$PWD"

echo "\
#!/bin/bash
set -e
git commit -a -m \"Change version to $1\"
git push
git checkout master
git merge develop
git tag $tag_version
git push origin --tags
git checkout develop
curl --data '$github_json' https://api.github.com/repos/actor-framework/actor-framework/releases?access_token=$token
cp "$blog_msg" "$blog_target_file"
cd ../blog
git add _posts
git commit -m \"$1 announcement\"
git push
cd "$anchor"
" > .make-release-steps.bash

echo ; echo
echo ">>> please review the final steps for releasing $1"
cat .make-release-steps.bash
echo ; echo
ask_permission "type [y] to execute the steps above or [n] to abort"

chmod +x .make-release-steps.bash
./.make-release-steps.bash

echo ; echo
echo ">>> cleaning up"
rm "$blog_msg" "$github_msg" .make-release-steps.bash

echo ; echo
echo ">>> done"
