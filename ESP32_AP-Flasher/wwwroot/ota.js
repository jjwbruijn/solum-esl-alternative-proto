var repo = apConfig.repo || 'OpenEPaperLink/OpenEPaperLink';
var repoUrl = 'https://api.github.com/repos/' + repo + '/releases';

const $ = document.querySelector.bind(document);

let running = false;
let errors = 0;
let env = '', currentVer = '', currentBuildtime = 0;
let buttonState = false;
let gIsC6 = false;
let gIsH2 = false;
let gModuleType = '';
let gShortName = '';
let gCurrentRfVer = 0;

export async function initUpdate() {
    if (apConfig.C6 == 1) {
        gIsC6 = true;
        gModuleType = "ESP32-C6";
        gShortName = "C6";
    }
    else if(apConfig?.H2 && apConfig.H2 == 1) {
        gIsH2 = true;
        gModuleType = "ESP32-H2";
        gShortName = "H2";
    }
    else {
        gModuleType = "Unknown"
    }
    $('#radio_release_title').innerHTML = gModuleType + " Firmware";

    const response = await fetch("version.txt");
    let filesystemversion = await response.text();
    if (!filesystemversion) filesystemversion = "unknown";
    $('#repo').value = repo;

    const envBox = $('#environment');
    if (envBox?.tagName === 'SELECT') {
        const inputElement = document.createElement('input');
        inputElement.type = 'text';
        inputElement.id = 'environment';
        envBox.parentNode.replaceChild(inputElement, envBox);
    }
    $('#environment').value = '';
    $('#environment').setAttribute('readonly', true);
    $('#repo').removeAttribute('readonly');
    $('#confirmSelectRepo').style.display = 'none';
    $('#cancelSelectRepo').style.display = 'none';
    $('#selectRepo').style.display = 'inline-block';
    $('#repoWarning').style.display = 'none';

    const sdata = await fetch("sysinfo")
        .then(response => {
            if (response.status != 200) {
                print("Error fetching sysinfo: " + response.status, "red");
                if (response.status == 404) {
                    print("Your current firmware version is not yet capable of updating OTA.");
                    print("Update it manually one last time.");
                    disableButtons(true);
                }
                return {};
            } else {
                return response.json();
            }
        })
        .catch(error => {
            print('Error fetching sysinfo: ' + error, "red");
        });

    if (sdata.env) {
        print(`current env:        ${sdata.env}`);
        print(`build date:         ${formatEpoch(sdata.buildtime)}`);
        print(`esp32 version:      ${sdata.buildversion}`);
        if(gModuleType != '') {
            var hex_ver = '0000' + sdata.ap_version.toString(16);
            print(`${gModuleType} version:   ${hex_ver.slice(-4)}`);
        }
        print(`filesystem version: ${filesystemversion}`);
        print(`psram size:         ${sdata.psramsize}`);
        print(`flash size:         ${sdata.flashsize}`);
        print("--------------------------", "gray");
        env = apConfig.env || sdata.env;
        if (sdata.env != env) {
            print(`Warning: you selected a build environment ${env} which is\ndifferent than the currently used ${sdata.env}.\nOnly update the firmware with a mismatched build environment if\nyou know what you're doing.`, "yellow");
        }
        currentVer = sdata.buildversion;
        currentBuildtime = sdata.buildtime;
        gCurrentRfVer = sdata.ap_version;
        if (sdata.rollback) $("#rollbackOption").style.display = 'block';
        $('#environment').value = env;
    }

	  const rdata = await fetch(repoUrl).then(response => response.json())
    const JsonName = 'firmware_' + gShortName + '.json';
    const releaseDetails = rdata.map(release => {
        const assets = release.assets;
        const filesJsonAsset = assets.find(asset => asset.name === 'filesystem.json');
        const binariesJsonAsset = assets.find(asset => asset.name === 'binaries.json');
        const containsEnv = assets.find(asset => asset.name === env + '.bin');
        const firmwareAsset = assets.find(asset => asset.name === JsonName);
        if (filesJsonAsset && binariesJsonAsset && containsEnv) {
            return {
                html_url: release.html_url,
                tag_name: release.tag_name,
                name: release.name,
                date: formatDateTime(release.published_at),
                author: release.author.login,
                file_url: filesJsonAsset.browser_download_url,
                bin_url: binariesJsonAsset.browser_download_url,
                firmware_url: firmwareAsset?.browser_download_url,
            }
        };
    })

    if (releaseDetails.length === 0) {
        easyupdate.innerHTML = ("No releases found.");
    } else {
        const release = releaseDetails[0];
        if (release?.tag_name) {
            if (release.tag_name == currentVer) {
                easyupdate.innerHTML = `Version ${currentVer}. You are up to date`;
            } else if (release.date < formatEpoch(currentBuildtime - 30 * 60)) {
                easyupdate.innerHTML = `Your version is newer than the latest release date.<br>Are you the developer? :-)`;
            } else {
                easyupdate.innerHTML = `An update from version ${currentVer} to version ${release.tag_name} is available.<button onclick="otamodule.updateAll('${release.bin_url}','${release.file_url}','${release.tag_name}')">Update now!</button>`;
            }
        }
    }

    const table = document.createElement('table');
    const tableHeader = document.createElement('tr');
    tableHeader.innerHTML = '<th>Release</th><th>Date</th><th>Name</th><th colspan="2"><center>Update</center></th><th>Remark</th>';
    table.appendChild(tableHeader);

    let rowCounter = 0;
    let radioFwCounter = 0;
    releaseDetails.forEach(release => {
        if (rowCounter < 4 && release?.html_url) {
            const tableRow = document.createElement('tr');
            let tablerow = `<td><a href="${release.html_url}" target="_new">${release.tag_name}</a></td><td>${release.date}</td><td>${release.name}</td><td><button type="button" onclick="otamodule.updateWebpage('${release.file_url}','${release.tag_name}', true)">Filesystem</button></td><td><button type="button" onclick="otamodule.updateESP('${release.bin_url}', true)">ESP32</button></td>`;
            if (release.tag_name == currentVer) {
                tablerow += "<td>current version</td>";
            } else if (release.date < formatEpoch(currentBuildtime)) {
                tablerow += "<td>older</td>";
            } else {
                tablerow += "<td>newer</td>";
            }
            tableRow.innerHTML = tablerow;
            table.appendChild(tableRow);
            rowCounter++;
        }
        if (release?.firmware_url) {
             radioFwCounter++;
        }
    });
    $('#releasetable').innerHTML = "";
    $('#releasetable').appendChild(table);

    if(radioFwCounter > 0) {
         const table1 = document.createElement('table');
         const tableHeader1 = document.createElement('tr');

         tableHeader1.innerHTML = '<th>Release</th><th>Date</th><th>Name</th><th><center>Update</center></th><th>Version</th><th>Remark</th>';
         table1.appendChild(tableHeader1);

         rowCounter = 0;
         for (const release of releaseDetails) {
             if (rowCounter < 4 && release?.firmware_url) {
                 const tableRow = document.createElement('tr');
                 var tablerow;
                 var firmwareVer = "unknown";
                 var release_url = release.firmware_url;

                tablerow = `<td><a href="${release.html_url}" target="_new">${release.tag_name}</a></td><td>${release.date}</td><td>${release.name}</td>`;
                tablerow += `<td><button type="button" onclick="otamodule.updateC6H2('${release_url}')">${gModuleType}</button></td>`;
                const firmwareUrl = 'http://proxy.openepaperlink.org/proxy.php?url=' + release.firmware_url;
                firmwareVer = await fetch(firmwareUrl, { method: 'GET'})
                    .then(function (response) { return response.json(); })
                    .then(function (response) { 
                        return response[2]['version']; })
                .catch(error => {
                    print('Error fetching releases:' + error, "red");
                });
                 tablerow += '<td>' + firmwareVer + '</td><td>';
                 if(firmwareVer != 'unknown') {
                     let Ver = Number('0x' + firmwareVer);
                     if(Ver > gCurrentRfVer) {
                         tablerow += 'Newer';
                     }
                     else if (Ver < gCurrentRfVer) {
                         tablerow += 'Older';
                     }
                     else if(!Number.isNaN(Ver)){
                         tablerow += 'Same';
                     }
                 }
                 tablerow += '</td>';
                 tableRow.innerHTML = tablerow;
                 table1.appendChild(tableRow);
                 rowCounter++;
             }
         };

         $('#radio_releasetable').innerHTML = "";
         $('#radio_releasetable').appendChild(table1);
    }

	const table2 = document.createElement('table');
	{
		const tableHeader2 = document.createElement('tr');
		tableHeader2.innerHTML = '<th>Firmware</th><th><center>Update</center></th>';
		table2.appendChild(tableHeader2);
		const tableRow = document.createElement('tr');
		tablerow = '<td>Last uploaded version</td>';
		tablerow += `<td><button type="button" onclick="otamodule.updateC6H2('')">${gModuleType}</button></td>`;
		tableRow.innerHTML = tablerow;
		table2.appendChild(tableRow);
	}
	{
		const tableRow = document.createElement('tr');
		const Url = "https://raw.githubusercontent.com/" + repo +
					"/master/binaries/ESP32-" + gShortName + 
					"/firmware_" + gShortName + ".json";

		tablerow = `<td><a href="https://github.com/${repo}" target="_new">Latest version from repo</a></td>`;
		tablerow += `<td><button type="button" onclick="otamodule.updateC6H2('${Url}')">${gModuleType}</button></td>`;
		tableRow.innerHTML = tablerow;
		table2.appendChild(tableRow);
	}
	$('#radio_releasetable1').innerHTML = "";
	$('#radio_releasetable1').appendChild(table2);

    disableButtons(buttonState);
}

export function updateAll(binUrl, fileUrl, tagname) {
    updateWebpage(fileUrl, tagname, false)
        .then(() => {
            fetchAndCheckTagtypes(true);
        })
        .then(() => {
            updateESP(binUrl, false);
        })
        .catch(error => {
            console.error(error);
        });
}

export async function updateWebpage(fileUrl, tagname, showReload) {
    return new Promise((resolve, reject) => {
        (async function () {
            try {
                if (running) return;
                if (showReload) {
                    if (!confirm("Confirm updating the filesystem")) return;
                } else {
                    if (!confirm("Confirm updating the esp32 and filesystem")) return;
                }

                disableButtons(true);
                running = true;
                errors = 0;
                const consoleDiv = document.getElementById('updateconsole');
                consoleDiv.scrollTop = consoleDiv.scrollHeight;

                print("Updating littleFS partition...");

                fetch("//openepaperlink.eu/getupdate/?url=" + fileUrl)
                    .then(response => response.json())
                    .then(data => {
                        checkfiles(data);
                    })
                    .catch(error => {
                        print('Error fetching data:' + error, "red");
                    });

                const checkfiles = async (files) => {
                    const updateactions = files.find(files => files.name === "update_actions.json");
                    if (updateactions) {
                        await fetchAndPost(updateactions.url, updateactions.name, updateactions.path);
                        try {
                            const response = await fetch("update_actions", {
                                method: "POST",
                                body: ''
                            });
                            if (response.ok) {
                                await response.text();
                            } else {
                                print(`error performing update actions: ${response.status}`, "red");
                                errors++;
                            }
                        } catch (error) {
                            console.error(`error calling update actions:` + error, "red");
                            errors++;
                        }
                    }

                    for (const file of files) {
                        try {
                            if (file.name != "update_actions.json") {
                                const url = "check_file?path=" + encodeURIComponent(file.path);
                                const response = await fetch(url);
                                if (response.ok) {
                                    const data = await response.json();
                                    if (data.filesize == file.size && data.md5 == file.md5) {
                                        print(`file ${file.path} is up to date`, "green");
                                    } else if (data.filesize == 0) {
                                        await fetchAndPost(file.url, file.name, file.path);
                                    } else {
                                        await fetchAndPost(file.url, file.name, file.path);
                                    }
                                } else {
                                    print(`error checking file ${file.path}: ${response.status}`, "red");
                                    errors++;
                                }
                            }
                        } catch (error) {
                            console.error(`error checking file ${file.path}:` + error, "red");
                            errors++;
                        }
                    }
                    writeVersion(tagname, "version.txt", "/www/version.txt")
                    running = false;
                    if (errors) {
                        print("------", "gray");
                        print(`Finished updating with ${errors} errors.`, "red");
                        reject(error);
                    } else {
                        print("------", "gray");
                        print("Update succesful.");
                        resolve();
                    }
                    disableButtons(false);

                    if (showReload) {
                        const newLine = document.createElement('div');
                        newLine.innerHTML = "<button onclick=\"location.reload()\">Reload this page</button>";
                        consoleDiv.appendChild(newLine);
                        consoleDiv.scrollTop = consoleDiv.scrollHeight;
                    }
                };
            } catch (error) {
                print('Error: ' + error, "red");
                errors++;
                reject(error);
            }
        })();
    });
}

export async function updateESP(fileUrl, showConfirm) {
    if (running) return;
    if (showConfirm) {
        if (!confirm("Confirm updating the esp32")) return;
    }

    disableButtons(true);
    running = true;
    errors = 0;
    const consoleDiv = document.getElementById('updateconsole');
    consoleDiv.scrollTop = consoleDiv.scrollHeight;

    print("Updating firmware...");

    let binurl, binmd5, binsize;

    let retryCount = 0;
    const maxRetries = 5;

    while (retryCount < maxRetries) {
        try {
            const response = await fetch("//openepaperlink.eu/getupdate/?url=" + fileUrl + "&env=" + env);
            const responseBody = await response.text();
            if (!response.ok) {
                throw new Error("Network response was not OK: " + responseBody);
            }

            if (!responseBody.trim().startsWith("[")) {
                throw new Error("Failed to fetch the release info file");
            }

            const data = JSON.parse(responseBody);
            const file = data.find((entry) => entry.name == env + '.bin');
            if (file) {
                binurl = "http://openepaperlink.eu/getupdate/?url=" + encodeURIComponent(file.url);
                binmd5 = file.md5;
                binsize = file.size;
                console.log(`URL for "${file.name}": ${binurl}`);

                try {
                    const response = await fetch('update_ota', {
                        method: 'POST',
                        headers: {
                            'Content-Type': 'application/x-www-form-urlencoded'
                        },
                        body: new URLSearchParams({
                            url: binurl,
                            md5: binmd5,
                            size: binsize
                        })
                    });

                    if (response.ok) {
                        await response.text();
                        print('OTA update initiated.');
                    } else {
                        print('Failed to initiate OTA update: ' + response.status, "red");
                    }
                } catch (error) {
                    print('Error during OTA update: ' + error, "red");
                }
                break;
            } else {
                print(`No info about "${env}" found in the release.`, "red");
            }
        } catch (error) {
            print('Error: ' + error.message, "yellow");
            retryCount++;
            print(`Retrying... attempt ${retryCount}`);
            await new Promise((resolve) => setTimeout(resolve, 3000));
        }
    }

    if (retryCount === maxRetries) {
        print("Reached maximum retry count. Failed to execute the update.", "red");
    }

    running = false;
    disableButtons(false);
}

$('#rollbackBtn').onclick = function () {
    if (running) return;

    disableButtons(true);
    running = true;
    errors = 0;
    const consoleDiv = document.getElementById('updateconsole');
    consoleDiv.scrollTop = consoleDiv.scrollHeight;

    print("Rolling back...");

    fetch("rollback", {
        method: "POST",
        body: ''
    })

    running = false;
    disableButtons(false);
}

export async function updateC6H2(Url) {
    if (running) return;
    disableButtons(true);
    running = true;
    errors = 0;
    const ReleaseUrl = Url.substring(0,Url.lastIndexOf('/'));
    const consoleDiv = document.getElementById('updateconsole');
    consoleDiv.scrollTop = consoleDiv.scrollHeight;
    const formData = new FormData();

    print("Flashing " + gModuleType + " ...");
    formData.append('url', ReleaseUrl);

    fetch("update_c6", {
        method: "POST",
        body: formData
    })

    running = false;
    disableButtons(false);
}

$('#updateTagtypeBtn').onclick = function () {
    const cleanup = $('#tagtype_clean').checked;
    fetchAndCheckTagtypes(cleanup);
}

$('#selectRepo').onclick = function (event) {
    event.preventDefault();
    $('#updateconsole').innerHTML = '';

    let repoUrl = 'https://api.github.com/repos/' + $('#repo').value + '/releases';
    fetch(repoUrl)
        .then(response => response.json())
        .then(data => {
            if (Array.isArray(data) && data.length > 0) {
                const release = data[0];
                print("Repo found! Latest release: " + release.name + " created " + release.created_at);
                const assets = release.assets;
                const filesJsonAsset = assets.find(asset => asset.name === 'filesystem.json');
                const binariesJsonAsset = assets.find(asset => asset.name === 'binaries.json');
                if (filesJsonAsset && binariesJsonAsset) {
                    const updateUrl = "//openepaperlink.eu/getupdate/?url=" + binariesJsonAsset.browser_download_url + "&env=" + $('#repo').value;
                    return fetch(updateUrl);
                } else {
                    throw new Error("Json file binaries.json and/or filesystem.json not found in the release assets");
                }            
            };
        })
        .then(updateResponse => {
            if (!updateResponse.ok) {
                throw new Error("Network response was not OK");
            }
            return updateResponse.text();
        })
        .then(responseBody => {
            if (!responseBody.trim().startsWith("[")) {
                throw new Error("Failed to fetch the release info file");
            }
            const updateData = JSON.parse(responseBody).filter(item => !item.name.endsWith('_full.bin') && !item.name.includes('_H2.') && !item.name.includes('_C6.'));

            const inputParent = $('#environment').parentNode;
            const selectElement = document.createElement('select');
            selectElement.id = 'environment';
            updateData.forEach(item => {
                const option = document.createElement('option');
                option.value = item.name.replace('.bin', '');
                option.text = item.name.replace('.bin', '');
                selectElement.appendChild(option);
            });
            inputParent.replaceChild(selectElement, $('#environment'));
            $('#environment').value = env;
            $('#confirmSelectRepo').style.display = 'inline-block';
            $('#cancelSelectRepo').style.display = 'inline-block';
            $('#selectRepo').style.display = 'none';
            $('#repo').setAttribute('readonly', true);
            $('#repoWarning').style.display = 'block';
        })
        .catch(error => {
            print('Error fetching releases:' + error, "red");
        });    
}

$('#cancelSelectRepo').onclick = function (event) {
    event.preventDefault();
    $('#updateconsole').innerHTML = '';
    initUpdate();
}

$('#confirmSelectRepo').onclick = function (event) {
    event.preventDefault();

    repo = $('#repo').value;
    let formData = new FormData();
    formData.append("repo", repo);
    formData.append("env", $('#environment').value);
    fetch("save_apcfg", {
        method: "POST",
        body: formData
    })
        .then(response => response.text())
        .then(data => {
            window.dispatchEvent(loadConfig);
            print('OK, Saved');
        })
        .catch(error => print('Error: ' + error));
    $('#updateconsole').innerHTML = '';
    repoUrl = 'https://api.github.com/repos/' + repo + '/releases';
    initUpdate();
}

export function print(line, color = "white") {
    const consoleDiv = document.getElementById('updateconsole');
    if (consoleDiv) {
        const isScrolledToBottom = consoleDiv.scrollHeight - consoleDiv.clientHeight <= consoleDiv.scrollTop;
        const newLine = document.createElement('div');
        newLine.style.color = color;
        if (line == "[reboot]") {
            newLine.innerHTML = "<button onclick=\"otamodule.reboot()\">Reboot</button>";
        } else {
            newLine.textContent = line;
        }
        consoleDiv.appendChild(newLine);
        if (isScrolledToBottom) {
            consoleDiv.scrollTop = consoleDiv.scrollHeight;
        }
    }
}

export function reboot() {
    print("Rebooting now... Reloading webpage in 5 seconds...", "yellow");
    fetch("reboot", { method: "POST" });
    setTimeout(() => {
        location.reload();
    }, 5000);
}

function formatEpoch(epochTime) {
    const date = new Date(epochTime * 1000); // Convert seconds to milliseconds

    const year = date.getFullYear();
    const month = String(date.getMonth() + 1).padStart(2, '0'); // Months are zero-based
    const day = String(date.getDate()).padStart(2, '0');
    const hours = String(date.getHours()).padStart(2, '0');
    const minutes = String(date.getMinutes()).padStart(2, '0');

    return `${year}-${month}-${day} ${hours}:${minutes}`;
}

function formatDateTime(utcDateString) {
    const localTimeZoneOffset = new Date().getTimezoneOffset();
    const date = new Date(utcDateString);
    date.setMinutes(date.getMinutes() - localTimeZoneOffset);
    const year = date.getUTCFullYear();
    const month = String(date.getUTCMonth() + 1).padStart(2, '0');
    const day = String(date.getUTCDate()).padStart(2, '0');
    const hours = String(date.getUTCHours()).padStart(2, '0');
    const minutes = String(date.getUTCMinutes()).padStart(2, '0');

    const formattedDate = `${year}-${month}-${day} ${hours}:${minutes}`;
    return formattedDate;
}

const fetchAndPost = async (url, name, path) => {
    try {
        print("updating " + path);
        const response = await fetch(url);

        if (!response.ok) {
            print(`download error: ${response.status} ${response.body}`, "red");
            errors++;
        } else {
            const fileContent = await response.blob();

            const formData = new FormData();
            formData.append('path', path);
            formData.append('file', fileContent, name);

            const uploadResponse = await fetch('littlefs_put', {
                method: 'POST',
                body: formData
            });

            if (!uploadResponse.ok) {
                print(`upload error: ${uploadResponse.status} ${uploadResponse.body}`, "red");
                errors++;
            }
        }
    } catch (error) {
        print('error: ' + error, "red");
        errors++;
    }
};

const writeVersion = async (content, name, path) => {
    try {
        print("uploading " + path);

        const formData = new FormData();
        formData.append('path', path);
        const blob = new Blob([content]);
        formData.append('file', blob, name);

        const uploadResponse = await fetch('littlefs_put', {
            method: 'POST',
            body: formData
        });

        if (!uploadResponse.ok) {
            print(`${response.status} ${response.body}`, "red");
            errors++;
        }
    } catch (error) {
        print('error: ' + error, "red");
        errors++;
    }
};

function disableButtons(active) {
    $("#configtab").querySelectorAll('button').forEach(button => {
        button.disabled = active;
    });
    buttonState = active;
}

async function fetchAndCheckTagtypes(cleanup) {
    print("Updating tagtype definitions...");
    const sortableGrid = $('#taglist');
    const gridItems = Array.from(sortableGrid.querySelectorAll('.tagcard:not(#tagtemplate)'));
    try {
        const response = await fetch('/edit?list=%2Ftagtypes');
        if (!response.ok) {
            print("Failed to fetch tagtypes list", "red");
            throw new Error('Failed to fetch tagtypes list');
        }
        const fileList = await response.json();

        for (const file of fileList) {
            const filename = file.name;
            print(filename, "green");
            let check = filename.endsWith('.json');
            let hwtype = parseInt(filename, 16);

            if (check && cleanup) {
                let isInUse = Array.from(gridItems).some(element => element.dataset.hwtype == hwtype);
                if (!isInUse) {
                    isInUse = Array.from(gridItems).some(element => element.dataset.usetemplate == hwtype);
                }
                if (!isInUse) {
                    print("not in use, deleting", "yellow");
                    const formData = new FormData();
                    formData.append('path', '/tagtypes/' + filename);
                    fetch('/edit', {
                        method: 'DELETE',
                        body: formData
                    })
                    check = false;
                }
            }
            
            if (check) {
                let githubUrl = "https://raw.githubusercontent.com/" + repo + "/master/resources/tagtypes/" + filename;

                const localResponse = await fetch(`/tagtypes/${filename}`);
                const localJson = await localResponse.json();
                const localVersion = localJson.version || 0;

                const githubResponse = await fetch(githubUrl);
                const githubJson = await githubResponse.json();
                const githubVersion = githubJson.version || 0;

                if (githubVersion > localVersion) {
                    print("update from version " + localVersion + " to " + githubVersion);
                    await fetchAndPost(githubUrl, filename, "/tagtypes/" + filename);
                }
            }
        }
        print("Finished.");
    } catch (error) {
        print("Error: " + error, "red");
    }
}

function normalizeVersion(version) {
    return version.replace(/(\.\d*?)0+$/, '$1').replace(/\.$/, '');
}
