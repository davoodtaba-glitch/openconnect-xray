#!/bin/sh

echo "This script will send the tarballs to infradead and create tags and release at gitlab"
echo "It will use your ssh keys and gitlab token as placed in ~/.gitlab-token"
echo "Press enter to continue..."
read

if test -z "$1";then
	echo "usage: $0 [VERSION]"
	echo "No version was specified"
	exit 1
fi

if ! test -f "$(expr ~/.gitlab-token)";then
	echo "Cannot find ~/.gitlab-token"
	exit 1
fi

if test -d "build";then
	echo "Please erase build/ before starting"
	exit 1
fi

PROJECT=12274423
TOKEN=$(cat ~/.gitlab-token)
version=$1
file=openconnect-gui-${version}.tar.xz
TAG=v${version}

echo ""
echo "Creating tag ${TAG}"
echo "Press enter to continue or type skip to skip..."
read s
if test "$s" != "s" && test "$s" != "skip";then
	set -e
	git tag -s ${TAG} -m "Released ${version}"
	git push origin ${TAG}
	set +e
fi

echo ""
echo "Waiting for builds of ${TAG}"
echo "Press enter to continue or type skip to skip..."
read s

if test "$s" != "s" && test "$s" != "skip";then
	set -e
	# commit id
	COMMIT_ID=$(curl -s --header "PRIVATE-TOKEN: $TOKEN" "https://gitlab.com/api/v4/projects/${PROJECT}/repository/tags/${TAG}" |jq '.commit.id'|tr -d '"')
	set +e

	status="running"
	trials=150
	while [[ $status != "success" && $trials > 0 ]];do
		sleep 30
		status=$(curl -s --header "PRIVATE-TOKEN: $TOKEN" "https://gitlab.com/api/v4/projects/${PROJECT}/repository/commits/${COMMIT_ID}"|jq '.last_pipeline.status'|tr -d '"')
		trials=$((trials-1))
		echo "Status: $status"
	done
fi

echo ""
echo "Building completed"

# Download
OUTFILE=$(mktemp)
set -e
curl -s --header "PRIVATE-TOKEN: $TOKEN" "https://gitlab.com/api/v4/projects/${PROJECT}/jobs/artifacts/${TAG}/download?job=WindowsRelease" --max-redirs 3 --location -o ${OUTFILE}
set +e

echo "Downloaded ${OUTFILE}"

unzip ${OUTFILE} && rm -f ${OUTFILE}

shafile=$(ls build/openconnect-gui-[0-9]*.[0-9]*.[0-9]*-win64.sha512)
file=$(ls build/openconnect-gui-[0-9]*.[0-9]*.[0-9]*-win64.exe)
if test -z "${file}";then
	echo "No exe file detected"
	exit 1
fi

if test -z "${shafile}";then
	echo "No checksum file detected"
	exit 1
fi

echo "Project ID: $PROJECT"
echo "Release file: $file"
echo "Checksum file: $shafile"

#echo $file|grep "$version" >/dev/null 2>&1
#if [[ $? != 0 ]];then
#	echo "File does not contain the release version ($version)"
#	exit 1
#fi

sha=$(cat ${shafile})

set -e
echo ""
echo "Validating checksum $sha"

cd build && echo $sha|sha512sum -c && cd ..
if [[ $? != 0 ]];then
	echo "File does not match the checksum"
	exit 1
fi

echo ""
echo "Identifying changelog"
msg=""
line=$(grep -n '## \[v'"${version}" CHANGELOG.md |cut -d ':' -f 1)
if test -n "$line";then
	stopline="$(head -n 100 CHANGELOG.md|tail -n $((100-$line))|grep -n '## \['|head -1|cut -d ':' -f 1)"
	msg=$(head -n 100 CHANGELOG.md|tail -n +$((1+$line))|head -n $(($stopline-1))|tr -d '"'|tr -d "'"|sed '{:q;N;s/\n/\\n/g;t q}')
fi
echo "Changelog: $msg"

echo ""
echo "Releasing exe file: $file"
echo "Press enter to continue or type skip to skip..."
read s

if test "$s" != "s" && test "$s" != "skip";then
	set -e
	scp ${file}* casper.infradead.org:/var/ftp/pub/openconnect-gui/
	set +e
fi

echo ""
echo "Checking for $version milestone"
echo "Press enter to continue or type skip to skip..."
read s

milestones=''
if test "$s" != "s" && test "$s" != "skip";then
	curl -s --header "PRIVATE-TOKEN: ${TOKEN}" "https://gitlab.com/api/v4/projects/${PROJECT}/milestones?title=$version"|grep "$version" >/dev/null 2>&1
	if [[ $? = 0 ]];then
		echo "Milestone $version will be linked to release"
		milestones=' "milestones": [ "'${version}'" ],'
	else
		echo "No matching milestones"
		milestones=''
	fi
fi

echo ""
echo "Posting release to gitlab project ${PROJECT}"
echo "Press enter to continue or type skip to skip..."
read s

basefile=$(basename $file)
if test "$s" != "s" && test "$s" != "skip";then
	released=0
	trials=3
	while [[ $released = 0 && $trials > 0 ]];do
		curl --header 'Content-Type: application/json' --header "PRIVATE-TOKEN: ${TOKEN}" \
		     --data '{ "name": "'${version}'", "tag_name": "v'${version}'", "description": "'"${msg}"'", '"${milestones}"' "assets": { "links": [{ "name": "Windows installer", "url": "https://www.infradead.org/openconnect-gui/download/'${basefile}'", "link_type":"package"} ] } }' \
		     --request POST "https://gitlab.com/api/v4/projects/${PROJECT}/releases" --no-progress-meter --fail-with-body
		if [[ $? = 0 ]];then
			released=1
		else
			echo ""
			sleep 15
			trials=$((trials-1))
			[[ $trials > 0 ]] && echo "Retrying release push"
		fi
	done

	if [[ $released = 0 ]];then
		echo "could not create release"
		exit 1
	fi
fi  

echo ""
echo "release is ready: https://gitlab.com/openconnect/openconnect-gui/-/releases"

exit 0
