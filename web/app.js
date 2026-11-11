const state = { path: "", file: null, uploading: false };

const elements = {
  breadcrumbs: document.querySelector("#breadcrumbs"),
  currentTitle: document.querySelector("#currentTitle"),
  itemCount: document.querySelector("#itemCount"),
  loading: document.querySelector("#loading"),
  emptyState: document.querySelector("#emptyState"),
  directorySection: document.querySelector("#directorySection"),
  directoryGrid: document.querySelector("#directoryGrid"),
  fileSection: document.querySelector("#fileSection"),
  fileTable: document.querySelector("#fileTable"),
  refreshButton: document.querySelector("#refreshButton"),
  uploadPath: document.querySelector("#uploadPath"),
  dropZone: document.querySelector("#dropZone"),
  fileInput: document.querySelector("#fileInput"),
  selectedFile: document.querySelector("#selectedFile"),
  progressWrap: document.querySelector("#progressWrap"),
  progressBar: document.querySelector("#progressBar"),
  progressText: document.querySelector("#progressText"),
  tokenInput: document.querySelector("#tokenInput"),
  uploadButton: document.querySelector("#uploadButton"),
  toast: document.querySelector("#toast")
};

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  const units = ["B", "KiB", "MiB", "GiB"];
  const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  return `${(bytes / (1024 ** index)).toFixed(index === 0 ? 0 : 2)} ${units[index]}`;
}

function showToast(message, isError = false) {
  elements.toast.textContent = message;
  elements.toast.classList.toggle("error", isError);
  elements.toast.classList.add("visible");
  window.clearTimeout(showToast.timer);
  showToast.timer = window.setTimeout(() => elements.toast.classList.remove("visible"), 3600);
}

function setSelectedFile(file) {
  if (file && !file.name.toLowerCase().endsWith(".zip")) {
    showToast("只能上传 ZIP 文件", true);
    file = null;
  }
  if (file && file.size > 2 * 1024 * 1024 * 1024) {
    showToast("文件超过 2 GiB 上传限制", true);
    file = null;
  }
  state.file = file || null;
  elements.fileInput.value = "";
  elements.selectedFile.hidden = !state.file;
  elements.selectedFile.textContent = state.file ? `${state.file.name} · ${formatBytes(state.file.size)}` : "";
  elements.uploadButton.disabled = !state.file || state.uploading;
}

function renderBreadcrumbs() {
  elements.breadcrumbs.replaceChildren();
  const parts = state.path ? state.path.split("/") : [];
  const rootButton = document.createElement("button");
  rootButton.className = "breadcrumb-button";
  rootButton.textContent = "update";
  rootButton.addEventListener("click", () => loadDirectory(""));
  elements.breadcrumbs.append(rootButton);

  parts.forEach((part, index) => {
    const separator = document.createElement("span");
    separator.className = "breadcrumb-separator";
    separator.textContent = "/";
    elements.breadcrumbs.append(separator);

    const button = document.createElement("button");
    button.className = "breadcrumb-button";
    button.textContent = part;
    button.addEventListener("click", () => loadDirectory(parts.slice(0, index + 1).join("/")));
    elements.breadcrumbs.append(button);
  });
}

function renderDirectory(data) {
  const directories = Array.isArray(data.directories) ? data.directories : [];
  const files = Array.isArray(data.files) ? data.files : [];
  elements.directoryGrid.replaceChildren();
  elements.fileTable.replaceChildren();

  directories.forEach(directory => {
    const button = document.createElement("button");
    button.className = "directory-card";
    const icon = document.createElement("span");
    icon.className = "folder-icon";
    icon.setAttribute("aria-hidden", "true");
    const name = document.createElement("span");
    name.className = "directory-name";
    name.textContent = directory.name;
    button.append(icon, name);
    button.addEventListener("click", () => loadDirectory(directory.path));
    elements.directoryGrid.append(button);
  });

  files.forEach(file => {
    const row = document.createElement("tr");
    const nameCell = document.createElement("td");
    nameCell.className = "file-name";
    nameCell.textContent = file.name;
    const versionCell = document.createElement("td");
    versionCell.textContent = file.version || "未识别";
    const sizeCell = document.createElement("td");
    sizeCell.textContent = formatBytes(Number(file.size));
    const actionCell = document.createElement("td");
    actionCell.className = "actions-column";
    const link = document.createElement("a");
    link.className = "download-link";
    link.textContent = "下载";
    link.href = `/api/files/download?path=${encodeURIComponent(file.path)}`;
    actionCell.append(link);
    row.append(nameCell, versionCell, sizeCell, actionCell);
    elements.fileTable.append(row);
  });

  const total = directories.length + files.length;
  elements.itemCount.textContent = `${total} 项`;
  elements.directorySection.hidden = directories.length === 0;
  elements.fileSection.hidden = files.length === 0;
  elements.emptyState.hidden = total !== 0;
}

async function loadDirectory(path) {
  if (state.uploading) return;
  elements.loading.hidden = false;
  elements.emptyState.hidden = true;
  elements.directorySection.hidden = true;
  elements.fileSection.hidden = true;
  try {
    const response = await fetch(`/api/files?path=${encodeURIComponent(path)}`, { cache: "no-store" });
    const data = await response.json();
    if (!response.ok) throw new Error(data.message || "目录读取失败");
    state.path = data.path || "";
    elements.currentTitle.textContent = state.path ? state.path.split("/").at(-1) : "update";
    elements.uploadPath.textContent = `/${state.path}`;
    renderBreadcrumbs();
    renderDirectory(data);
    setSelectedFile(null);
  } catch (error) {
    elements.emptyState.hidden = false;
    showToast(error.message || "目录读取失败", true);
  } finally {
    elements.loading.hidden = true;
  }
}

function uploadFile() {
  if (!state.file || state.uploading) return;
  state.uploading = true;
  elements.uploadButton.disabled = true;
  elements.progressWrap.hidden = false;
  elements.progressBar.style.width = "0%";
  elements.progressText.textContent = "0%";

  const form = new FormData();
  form.append("file", state.file, state.file.name);
  const xhr = new XMLHttpRequest();
  xhr.open("POST", `/api/files/upload?path=${encodeURIComponent(state.path)}`);
  const token = elements.tokenInput.value.trim();
  if (token) xhr.setRequestHeader("Authorization", `Bearer ${token}`);
  xhr.upload.addEventListener("progress", event => {
    if (!event.lengthComputable) return;
    const percent = Math.min(100, Math.round((event.loaded / event.total) * 100));
    elements.progressBar.style.width = `${percent}%`;
    elements.progressText.textContent = `${percent}%`;
  });
  xhr.addEventListener("load", async () => {
    state.uploading = false;
    let result = {};
    try { result = JSON.parse(xhr.responseText); } catch (_) { /* binary/empty response */ }
    if (xhr.status >= 200 && xhr.status < 300) {
      showToast(`${result.filename || state.file.name} 上传成功`);
      await loadDirectory(state.path);
    } else {
      elements.uploadButton.disabled = false;
      showToast(result.message || `上传失败（HTTP ${xhr.status}）`, true);
    }
  });
  xhr.addEventListener("error", () => {
    state.uploading = false;
    elements.uploadButton.disabled = false;
    showToast("网络错误，上传失败", true);
  });
  xhr.send(form);
}

elements.refreshButton.addEventListener("click", () => loadDirectory(state.path));
elements.fileInput.addEventListener("change", event => setSelectedFile(event.target.files[0]));
elements.uploadButton.addEventListener("click", uploadFile);
elements.tokenInput.value = sessionStorage.getItem("codeitUploadToken") || "";
elements.tokenInput.addEventListener("input", () => sessionStorage.setItem("codeitUploadToken", elements.tokenInput.value));
["dragenter", "dragover"].forEach(type => elements.dropZone.addEventListener(type, event => {
  event.preventDefault();
  elements.dropZone.classList.add("dragging");
}));
["dragleave", "drop"].forEach(type => elements.dropZone.addEventListener(type, event => {
  event.preventDefault();
  elements.dropZone.classList.remove("dragging");
}));
elements.dropZone.addEventListener("drop", event => setSelectedFile(event.dataTransfer.files[0]));

loadDirectory("");
