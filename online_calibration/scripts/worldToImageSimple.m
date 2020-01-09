function image_points = worldToImageSimple(camera_params, camframe_ptd)

n = length(camframe_ptd(:, 1));
image_points(1:n, 1:2) = double(0);

for i = 1:n
    point = camframe_ptd(i, :)';
    uv = camera_params.intrinsic_matrix_ * point;
    
    image_points(i, 1) = uv(1) / point(3);
    image_points(i, 2) = uv(2) / point(3);
end

