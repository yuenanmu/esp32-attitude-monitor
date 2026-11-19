import * as THREE from "./vendor/three.module.min.js";

// Device NWU is Z-up; the scene is Y-up. Device (x,y,z) maps to (x,z,-y).
const deviceBasis = new THREE.Quaternion().setFromAxisAngle(
  new THREE.Vector3(1, 0, 0), -Math.PI / 2
);
const inverseBasis = deviceBasis.clone().invert();

export function attitudeQuaternion({ roll, pitch, yaw }) {
  const rad = THREE.MathUtils.degToRad;
  const euler = new THREE.Euler(rad(roll), rad(pitch), rad(yaw), "ZYX");
  return deviceBasis.clone()
    .multiply(new THREE.Quaternion().setFromEuler(euler))
    .multiply(inverseBasis);
}

function boardModel() {
  const board = new THREE.Group();
  function box(w, h, d, color, x = 0, y = 0, z = 0, emissive = 0) {
    const geometry = new THREE.BoxGeometry(w, h, d);
    const mesh = new THREE.Mesh(geometry, new THREE.MeshStandardMaterial({
      color, metalness: 0.18, roughness: 0.54, emissive
    }));
    mesh.position.set(x, y, z);
    board.add(mesh);
    return mesh;
  }
  box(3.00, 0.15, 2.18, 0x16766c);
  box(2.51, 0.12, 1.94, 0x263647, 0, 0.124);
  box(2.29, 0.025, 1.72, 0x102b42, 0, 0.191, 0x072139);
  box(1.61, 0.012, 0.035, 0x55decf, 0.01, 0.21, 0.09, 0x1b7c70);
  box(0.033, 0.012, 1.12, 0x305b78, 0, 0.21, 0.02);
  box(0.46, 0.015, 0.165, 0x55decf, 0.04, 0.213, -0.37, 0x1b7c70);
  box(0.66, 0.24, 0.52, 0xaeb9c4, -1.48, 0, 0);
  box(0.015, 0.12, 0.36, 0x101923, -1.815, 0, 0);
  box(0.14, 0.012, 0.11, 0x55decf, 1.37, 0.081, 0.72, 0x28a296);
  for (const x of [-1.34, 1.34]) {
    for (const z of [-0.93, 0.93]) {
      const pad = new THREE.Mesh(
        new THREE.RingGeometry(0.042, 0.081, 24),
        new THREE.MeshStandardMaterial({ color: 0xe4bc71, side: THREE.DoubleSide })
      );
      pad.rotation.x = -Math.PI / 2;
      pad.position.set(x, 0.078, z);
      board.add(pad);
    }
  }
  for (const [direction, color] of [
    [[1, 0, 0], 0x55decf],
    [[0, 0, -1], 0xffc878],
    [[0, 1, 0], 0xb9a1ff],
  ]) {
    board.add(new THREE.ArrowHelper(new THREE.Vector3(...direction),
      new THREE.Vector3(0, 0.24, 0), 1.18, color, 0.20, 0.13));
  }
  return board;
}

export function createPoseView(host, resetButton) {
  const renderer = new THREE.WebGLRenderer({ alpha: true, antialias: true });
  renderer.setPixelRatio(Math.min(devicePixelRatio || 1, 2));
  host.append(renderer.domElement);
  const canvas = renderer.domElement;
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(42, 1, 0.1, 50);
  const board = boardModel();
  scene.add(board, new THREE.HemisphereLight(0xd4ecff, 0x405872, 2));
  const key = new THREE.DirectionalLight(0xe1f9ff, 2.5);
  key.position.set(-3, 6, 4);
  scene.add(key);
  const grid = new THREE.GridHelper(16, 32, 0x314e62, 0x203344);
  grid.position.y = -1.95;
  scene.add(grid);

  const target = new THREE.Quaternion();
  const reduceMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
  let azimuth, elevation, distance, drag = null;
  function resetCamera() {
    azimuth = 0.64; elevation = 0.56; distance = 6.4;
  }
  resetCamera();
  resetButton.disabled = false;
  resetButton.addEventListener("click", resetCamera);
  canvas.addEventListener("pointerdown", (event) => {
    if (drag || event.button !== 0) return;
    drag = { id: event.pointerId, x: event.clientX, y: event.clientY };
    canvas.setPointerCapture(event.pointerId);
  });
  canvas.addEventListener("pointermove", (event) => {
    if (drag?.id !== event.pointerId) return;
    azimuth -= (event.clientX - drag.x) * 0.009;
    elevation = THREE.MathUtils.clamp(elevation + (event.clientY - drag.y) * 0.009, -1.4, 1.4);
    drag.x = event.clientX;
    drag.y = event.clientY;
  });
  for (const name of ["pointerup", "pointercancel", "lostpointercapture"]) {
    canvas.addEventListener(name, (event) => {
      if (drag?.id === event.pointerId) drag = null;
    });
  }
  canvas.addEventListener("wheel", (event) => {
    event.preventDefault();
    distance = THREE.MathUtils.clamp(distance * Math.exp(event.deltaY * 0.0014), 3.3, 12);
  }, { passive: false });

  function resize() {
    const width = host.clientWidth;
    const height = host.clientHeight;
    if (!width || !height) return;
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
    renderer.setSize(width, height, false);
  }
  resize();
  new ResizeObserver(resize).observe(host);
  let previous = performance.now();
  renderer.setAnimationLoop((now) => {
    const elapsed = Math.min(Math.max(now - previous, 0), 100);
    previous = now;
    board.quaternion.slerp(target, reduceMotion ? 1 : 1 - Math.exp(-elapsed / 85));
    camera.position.set(
      distance * Math.cos(elevation) * Math.sin(azimuth), distance * Math.sin(elevation),
      distance * Math.cos(elevation) * Math.cos(azimuth)
    );
    camera.lookAt(0, 0, 0);
    renderer.render(scene, camera);
  });
  return {
    setAngles(sample) { target.copy(attitudeQuaternion(sample)); },
    clear() { target.identity(); board.quaternion.identity(); },
  };
}
