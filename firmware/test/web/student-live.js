export function renderLatest(latest, renderSample) {
  // 这是你要写的唯一 TODO。latest 是 /api/state 返回的 state.latest。
  // TODO：
  // 1. latest 为 null 时，直接 return。
  // 2. 取出 seq、t_us、roll、pitch、yaw。
  // 3. 用这五个字段组成一个对象，调用 renderSample(对象)。
  //    数字卡片、趋势图和 3D 模型会一起更新。
  if (latest === null) {
      return;
  }
  const {seq, t_us, roll, pitch, yaw } = latest;
  renderSample({ seq, t_us, roll, pitch, yaw });
  return;
}
